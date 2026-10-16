#include "WaveSession.h"

#include "../debug/DebugArtifactIO.h"
#include "eda-platform/Sha256.h"
#include <json/json.h>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace sigflow::wave {
namespace {

constexpr std::uintmax_t kMaxSessionBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaxEntries = 100000;

const char* KindName(sigflow::trace::SignalKind kind)
{
    switch (kind) {
        case sigflow::trace::SignalKind::Scalar: return "scalar";
        case sigflow::trace::SignalKind::Vector: return "vector";
        case sigflow::trace::SignalKind::Real: return "real";
        case sigflow::trace::SignalKind::String: return "string";
    }
    return "invalid";
}

bool ValidIdentity(const WaveSourceIdentity& identity)
{
    return identity.sha256.size() == 64 && std::all_of(identity.sha256.begin(), identity.sha256.end(),
        [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool ParseSession(const Json::Value& root, WaveSessionData& out, std::string& error)
{
    if (!root.isObject()) { error = "wave session must be an object"; return false; }
    WaveSessionData parsed;
    auto text = [&](const Json::Value& object, const char* name, std::string& value) {
        if (!object.isMember(name)) return true;
        if (!object[name].isString()) { error = std::string("invalid session string: ") + name; return false; }
        value = object[name].asString();
        return true;
    };
    auto number = [&](const char* name, std::uint64_t& value) {
        if (!root.isMember(name)) return true;
        if (!root[name].isUInt64()) { error = std::string("invalid session time: ") + name; return false; }
        value = root[name].asUInt64();
        return true;
    };
    auto flag = [&](const char* name, bool& value) {
        if (!root.isMember(name)) return true;
        if (!root[name].isBool()) { error = std::string("invalid session flag: ") + name; return false; }
        value = root[name].asBool();
        return true;
    };
    auto array = [&](const char* name) {
        if (!root.isMember(name)) return true;
        if (!root[name].isArray() || root[name].size() > kMaxEntries) {
            error = std::string("invalid or oversized session array: ") + name; return false;
        }
        return true;
    };
    parsed.schemaVersion = "1.0";
    if (!text(root, "schema_version", parsed.schemaVersion) ||
        (parsed.schemaVersion != "1.0" && parsed.schemaVersion != "2.0")) {
        if (error.empty()) error = "unsupported wave session version";
        return false;
    }
    std::string theme = "dark";
    if (!text(root, "source_path", parsed.sourcePath) || parsed.sourcePath.empty() ||
        !text(root, "theme", theme) || (theme != "dark" && theme != "light") ||
        !text(root, "uart_capture_path", parsed.uartCapturePath) ||
        !number("time_offset", parsed.timeOffset) || !number("time_span", parsed.timeSpan) ||
        !number("playhead", parsed.playhead) || !number("ab_a", parsed.abA) || !number("ab_b", parsed.abB) ||
        !flag("has_playhead", parsed.hasPlayhead) || !flag("has_ab", parsed.hasAB) || parsed.timeSpan == 0) {
        if (error.empty()) error = "invalid wave session header";
        return false;
    }
    parsed.theme = ThemeFromName(theme);
    if (root.isMember("source_identity")) {
        const auto& identity = root["source_identity"];
        if (!identity.isObject() || !identity["sha256"].isString() || !identity["byte_size"].isUInt64()) {
            error = "invalid wave source identity"; return false;
        }
        parsed.sourceIdentity.sha256 = identity["sha256"].asString();
        parsed.sourceIdentity.byteSize = identity["byte_size"].asUInt64();
        if (!ValidIdentity(parsed.sourceIdentity)) { error = "invalid wave source SHA-256"; return false; }
    }
    if (parsed.schemaVersion == "2.0") {
        if (!ValidIdentity(parsed.sourceIdentity) || !root["signal_bindings"].isArray() ||
            !array("signal_bindings")) { error = "version 2 requires source identity and stable signal bindings"; return false; }
        std::set<std::string> keys;
        for (const auto& item : root["signal_bindings"]) {
            WaveSignalBinding binding;
            std::string kind;
            std::string displayFormat = "binary";
            if (!item.isObject() || !text(item, "signal_key", binding.signalKey) || binding.signalKey.empty() ||
                !keys.insert(binding.signalKey).second || !item["width"].isUInt() || item["width"].asUInt() == 0 ||
                !item["visible"].isBool() || !text(item, "kind", kind) ||
                !text(item, "alias", binding.alias) || !text(item, "comment", binding.comment) ||
                !text(item, "display_format", displayFormat) ||
                !WaveDisplayFormatFromName(displayFormat, binding.displayFormat)) {
                error = "invalid or duplicate stable signal binding"; return false;
            }
            binding.width = item["width"].asUInt();
            binding.visible = item["visible"].asBool();
            if (kind == "scalar") binding.kind = sigflow::trace::SignalKind::Scalar;
            else if (kind == "vector") binding.kind = sigflow::trace::SignalKind::Vector;
            else if (kind == "real") binding.kind = sigflow::trace::SignalKind::Real;
            else if (kind == "string") binding.kind = sigflow::trace::SignalKind::String;
            else { error = "unsupported signal kind in wave session"; return false; }
            parsed.signalBindings.push_back(std::move(binding));
        }
    } else {
        if (!array("signals")) return false;
        std::set<int> ids;
        for (const auto& item : root["signals"]) {
            if (!item.isInt() || item.asInt() < 0 || !ids.insert(item.asInt()).second) {
                error = "invalid or duplicate legacy signal ID"; return false;
            }
            parsed.visibleSignalIds.push_back(item.asInt());
        }
        auto readMetadata = [&](const char* name, std::unordered_map<int, std::string>& values) {
            if (!root.isMember(name)) return true;
            if (!root[name].isObject() || root[name].size() > kMaxEntries) { error = "invalid legacy signal metadata"; return false; }
            for (const auto& key : root[name].getMemberNames()) {
                int id = -1;
                const auto result = std::from_chars(key.data(), key.data() + key.size(), id);
                if (result.ec != std::errc() || result.ptr != key.data() + key.size() || id < 0 ||
                    std::to_string(id) != key || !root[name][key].isString()) {
                    error = "invalid legacy metadata ID or value"; return false;
                }
                values.emplace(id, root[name][key].asString());
            }
            return true;
        };
        if (!readMetadata("signal_aliases", parsed.signalAliases) ||
            !readMetadata("signal_comments", parsed.signalComments)) return false;
    }
    if (!array("markers") || !array("events")) return false;
    for (const auto& item : root["markers"]) {
        WaveMarker marker;
        if (!item.isObject() || !item["time"].isUInt64() || !item["color"].isUInt() || !text(item, "label", marker.label)) {
            error = "invalid wave marker"; return false;
        }
        marker.time = item["time"].asUInt64();
        marker.color = item["color"].asUInt();
        parsed.markers.push_back(std::move(marker));
    }
    for (const auto& item : root["events"]) {
        WaveEvent event;
        if (!item.isObject() || !item["time"].isUInt64() || !item["color"].isUInt() ||
            !text(item, "label", event.label) || !text(item, "signal_name", event.signalName) ||
            !text(item, "source_path", event.sourcePath) || !item["source_line"].isInt() || item["source_line"].asInt() < 0) {
            error = "invalid wave event"; return false;
        }
        event.time = item["time"].asUInt64();
        event.color = item["color"].asUInt();
        event.sourceLine = item["source_line"].asInt();
        parsed.events.push_back(std::move(event));
    }
    out = std::move(parsed);
    return true;
}

bool SameFile(const std::filesystem::path& left, const std::filesystem::path& right)
{
    std::error_code ec;
    if (std::filesystem::equivalent(left, right, ec) && !ec) return true;
    const auto a = std::filesystem::weakly_canonical(left, ec);
    if (ec) return false;
    const auto b = std::filesystem::weakly_canonical(right, ec);
    return !ec && a == b;
}

} // namespace

bool WaveSourceFingerprint(const std::string& path, WaveSourceIdentity& identity, std::string& error)
{
    error.clear();
    const auto file = std::filesystem::u8path(path);
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) { error = "unable to inspect wave source: " + path; return false; }
    const auto modified = std::filesystem::last_write_time(file, ec);
    if (ec) { error = "unable to inspect wave source timestamp"; return false; }
    WaveSourceIdentity current;
    current.sha256 = eda::platform::Sha256FileHex(file);
    current.byteSize = size;
    current.modifiedTicks = modified.time_since_epoch().count();
    const auto afterSize = std::filesystem::file_size(file, ec);
    if (ec || size != afterSize) { error = "wave source changed during fingerprinting"; return false; }
    const auto afterTime = std::filesystem::last_write_time(file, ec);
    if (ec || modified != afterTime || !ValidIdentity(current)) { error = "wave source unreadable or changed during fingerprinting"; return false; }
    identity = std::move(current);
    return true;
}

bool WaveSourceIdentityMatches(const WaveSourceIdentity& left, const WaveSourceIdentity& right)
{
    return ValidIdentity(left) && ValidIdentity(right) && left.sha256 == right.sha256 && left.byteSize == right.byteSize;
}

bool WaveSessionBindSignals(const sigflow::trace::TraceSource& source, WaveSessionData& session, std::string& error)
{
    error.clear();
    std::vector<WaveSignalBinding> bindings;
    std::set<std::string> used;
    std::map<std::string, WaveDisplayFormat> previousFormats;
    for (const auto& binding : session.signalBindings) {
        if (!binding.signalKey.empty()) previousFormats[binding.signalKey] = binding.displayFormat;
    }
    std::map<std::string, std::vector<const sigflow::trace::SignalInfo*>> catalogue;
    for (const auto& signal : source.Signals()) catalogue[signal.fullName].push_back(&signal);
    auto append = [&](int id, bool visible) {
        const auto* signal = source.SignalById(id);
        if (!signal || signal->fullName.empty() || signal->width == 0) { error = "session refers to an unknown signal ID"; return false; }
        if (catalogue[signal->fullName].size() != 1) { error = "ambiguous full signal path in trace source"; return false; }
        if (!used.insert(signal->fullName).second) { error = "duplicate stable signal key in session"; return false; }
        WaveSignalBinding binding{signal->fullName, signal->width, signal->kind, visible};
        if (const auto previous = previousFormats.find(signal->fullName);
            previous != previousFormats.end()) {
            binding.displayFormat = previous->second;
        }
        if (const auto runtime = session.signalDisplayFormats.find(id);
            runtime != session.signalDisplayFormats.end()) {
            binding.displayFormat = runtime->second;
        }
        if (auto item = session.signalAliases.find(id); item != session.signalAliases.end()) binding.alias = item->second;
        if (auto item = session.signalComments.find(id); item != session.signalComments.end()) binding.comment = item->second;
        bindings.push_back(std::move(binding));
        return true;
    };
    std::set<int> visible;
    for (int id : session.visibleSignalIds) {
        if (!visible.insert(id).second || !append(id, true)) {
            if (error.empty()) error = "duplicate visible signal ID";
            return false;
        }
    }
    std::map<std::string, int> hidden;
    for (const auto* values : {&session.signalAliases, &session.signalComments}) {
        for (const auto& [id, value] : *values) {
            if (visible.count(id)) continue;
            const auto* signal = source.SignalById(id);
            if (!signal) { error = "metadata refers to an unknown signal ID"; return false; }
            hidden.emplace(signal->fullName, id);
        }
    }
    for (const auto& [key, id] : hidden) if (!append(id, false)) return false;
    std::set<std::string> currentVisible;
    for (const auto& binding : bindings) if (binding.visible) currentVisible.insert(binding.signalKey);
    std::map<std::string, std::string> anchors;
    std::string nextVisible;
    for (auto it = session.signalBindings.rbegin(); it != session.signalBindings.rend(); ++it) {
        if (currentVisible.count(it->signalKey)) nextVisible = it->signalKey;
        else anchors[it->signalKey] = nextVisible;
    }
    std::map<std::string, std::vector<WaveSignalBinding>> unresolvedBefore;
    std::vector<WaveSignalBinding> oldHidden;
    for (auto binding : session.signalBindings) {
        if (used.count(binding.signalKey)) continue;
        const auto found = catalogue.find(binding.signalKey);
        if (found != catalogue.end() && found->second.size() == 1 &&
            found->second.front()->width == binding.width && found->second.front()->kind == binding.kind) {
            binding.visible = false;
            binding.alias.clear();
            binding.comment.clear();
        }
        used.insert(binding.signalKey);
        if (binding.visible) unresolvedBefore[anchors[binding.signalKey]].push_back(std::move(binding));
        else oldHidden.push_back(std::move(binding));
    }
    // Keep an unresolved row in its old slot before the next surviving row.
    // A save/reopen cycle must not move a missing middle row to the bottom.
    std::vector<WaveSignalBinding> ordered;
    auto insertUnresolved = [&](const std::string& anchor) {
        auto found = unresolvedBefore.find(anchor);
        if (found == unresolvedBefore.end()) return;
        for (auto& binding : found->second) ordered.push_back(std::move(binding));
    };
    for (auto& binding : bindings) if (binding.visible) {
        insertUnresolved(binding.signalKey);
        ordered.push_back(std::move(binding));
    }
    insertUnresolved("");
    for (auto& binding : bindings) if (!binding.visible) ordered.push_back(std::move(binding));
    for (auto& binding : oldHidden) ordered.push_back(std::move(binding));
    session.schemaVersion = "2.0";
    session.sourcePath = source.Path();
    session.signalBindings = std::move(ordered);
    session.signalDisplayFormats.clear();
    return true;
}

bool WaveSessionResolve(const WaveSessionData& saved, const sigflow::trace::TraceSource& source,
                        const WaveSourceIdentity& current, bool layoutOnly,
                        WaveSessionResolution& result, std::string& error)
{
    error.clear();
    WaveSessionResolution resolved;
    if (!ValidIdentity(current) || (saved.schemaVersion == "2.0" && !ValidIdentity(saved.sourceIdentity))) {
        error = "wave session resolution requires a valid source identity"; return false;
    }
    resolved.sourceMatched = WaveSourceIdentityMatches(saved.sourceIdentity, current);
    if (!resolved.sourceMatched && (!layoutOnly || saved.schemaVersion != "2.0")) {
        error = saved.schemaVersion == "1.0"
            ? "legacy numeric session requires a matching source SHA-256; path alone cannot verify IDs"
            : "wave source differs from saved identity; only explicit layout-only restoration is allowed";
        return false;
    }
    resolved.session = saved;
    if (saved.schemaVersion == "1.0") {
        if (!WaveSessionBindSignals(source, resolved.session, error)) return false;
        resolved.legacyMigrated = true;
    } else if (saved.schemaVersion != "2.0") { error = "unsupported wave session version"; return false; }
    auto& session = resolved.session;
    session.visibleSignalIds.clear();
    session.signalAliases.clear();
    session.signalComments.clear();
    session.signalDisplayFormats.clear();
    std::map<std::string, std::vector<const sigflow::trace::SignalInfo*>> signals;
    for (const auto& signal : source.Signals()) signals[signal.fullName].push_back(&signal);
    std::set<std::string> seen;
    for (const auto& binding : session.signalBindings) {
        if (binding.signalKey.empty() || !seen.insert(binding.signalKey).second) { error = "duplicate or empty stable signal key"; return false; }
        const auto found = signals.find(binding.signalKey);
        if (found == signals.end() || found->second.size() != 1 || found->second.front()->width != binding.width ||
            found->second.front()->kind != binding.kind) {
            resolved.unresolved.push_back(binding);
            continue;
        }
        const int id = found->second.front()->id;
        if (binding.visible) session.visibleSignalIds.push_back(id);
        if (!binding.alias.empty()) session.signalAliases[id] = binding.alias;
        if (!binding.comment.empty()) session.signalComments[id] = binding.comment;
        session.signalDisplayFormats[id] = binding.displayFormat;
    }
    if (!resolved.sourceMatched || layoutOnly) {
        session.timeOffset = 0;
        session.timeSpan = 1000;
        session.markers.clear();
        session.events.clear();
        session.playhead = session.abA = session.abB = 0;
        session.hasPlayhead = session.hasAB = false;
        session.uartCapturePath.clear();
    }
    session.schemaVersion = "2.0";
    session.sourcePath = source.Path();
    session.sourceIdentity = current;
    result = std::move(resolved);
    return true;
}

bool WaveSessionSave(const WaveSessionData& session, const std::string& path, std::string& error)
{
    error.clear();
    if (session.schemaVersion != "2.0") { error = "resolve legacy session before saving version 2"; return false; }
    if (session.signalBindings.empty() && (!session.visibleSignalIds.empty() ||
        !session.signalAliases.empty() || !session.signalComments.empty())) {
        error = "bind numeric signal metadata to stable paths before saving"; return false;
    }
    const auto destination = std::filesystem::u8path(path);
    const auto source = std::filesystem::u8path(session.sourcePath);
    auto temporary = destination;
    temporary += ".tmp";
    if (SameFile(destination, source) || SameFile(temporary, source)) {
        error = "wave session must not overwrite its trace source"; return false;
    }
    Json::Value root(Json::objectValue);
    root["schema_version"] = "2.0";
    root["source_path"] = session.sourcePath;
    root["source_identity"]["sha256"] = session.sourceIdentity.sha256;
    root["source_identity"]["byte_size"] = Json::UInt64(session.sourceIdentity.byteSize);
    root["signal_bindings"] = Json::Value(Json::arrayValue);
    for (const auto& binding : session.signalBindings) {
        Json::Value item(Json::objectValue);
        item["signal_key"] = binding.signalKey;
        item["width"] = binding.width;
        item["kind"] = KindName(binding.kind);
        item["visible"] = binding.visible;
        item["alias"] = binding.alias;
        item["comment"] = binding.comment;
        item["display_format"] = WaveDisplayFormatName(binding.displayFormat);
        root["signal_bindings"].append(item);
    }
    root["time_offset"] = Json::UInt64(session.timeOffset);
    root["time_span"] = Json::UInt64(session.timeSpan);
    root["playhead"] = Json::UInt64(session.playhead);
    root["has_playhead"] = session.hasPlayhead;
    root["ab_a"] = Json::UInt64(session.abA);
    root["ab_b"] = Json::UInt64(session.abB);
    root["has_ab"] = session.hasAB;
    root["theme"] = ThemeName(session.theme);
    root["uart_capture_path"] = session.uartCapturePath;
    root["markers"] = Json::Value(Json::arrayValue);
    for (const auto& marker : session.markers) {
        Json::Value item(Json::objectValue);
        item["time"] = Json::UInt64(marker.time);
        item["label"] = marker.label;
        item["color"] = marker.color;
        root["markers"].append(item);
    }
    root["events"] = Json::Value(Json::arrayValue);
    for (const auto& event : session.events) {
        Json::Value item(Json::objectValue);
        item["time"] = Json::UInt64(event.time);
        item["label"] = event.label;
        item["color"] = event.color;
        item["signal_name"] = event.signalName;
        item["source_path"] = event.sourcePath;
        item["source_line"] = event.sourceLine;
        root["events"].append(item);
    }
    WaveSessionData validated;
    if (!ParseSession(root, validated, error)) return false;
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    const auto content = Json::writeString(writer, root) + "\n";
    if (content.size() > kMaxSessionBytes) { error = "wave session exceeds 16 MiB limit"; return false; }
    return sigflow::debug::WriteTextFileAtomic(destination, content, error);
}

bool WaveSessionLoad(const std::string& path, WaveSessionData& session, std::string& error)
{
    error.clear();
    const auto file = std::filesystem::u8path(path);
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec || size > kMaxSessionBytes) { error = "wave session unreadable or exceeds 16 MiB limit"; return false; }
    std::ifstream input(file, std::ios::binary);
    if (!input) { error = "unable to open wave session: " + path; return false; }
    std::string content(static_cast<std::size_t>(size), '\0');
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (input.bad() || input.gcount() != static_cast<std::streamsize>(content.size()) || input.peek() != EOF) {
        error = "wave session changed or failed while reading"; return false;
    }
    std::istringstream stream(content);
    Json::Value root;
    Json::CharReaderBuilder reader;
    reader["rejectDupKeys"] = true;
    reader["failIfExtra"] = true;
    try {
        if (!Json::parseFromStream(reader, stream, &root, &error)) return false;
        return ParseSession(root, session, error);
    } catch (const std::exception& exception) {
        error = std::string("invalid wave session: ") + exception.what();
        return false;
    }
}

} // namespace sigflow::wave
