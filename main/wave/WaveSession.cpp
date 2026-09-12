#include "WaveSession.h"

#include <json/json.h>

#include <fstream>
#include <memory>

namespace sigflow {
namespace wave {

namespace {

std::string JsonEscape(const std::string& value)
{
    Json::Value node(value);
    return node.asString();
}

} // namespace

bool WaveSessionSave(const WaveSessionData& session, const std::string& path,
                     std::string& error)
{
    Json::Value root(Json::objectValue);
    root["schema_version"] = "1.0";
    root["source_path"] = session.sourcePath;
    root["time_offset"] = static_cast<Json::UInt64>(session.timeOffset);
    root["time_span"] = static_cast<Json::UInt64>(session.timeSpan);

    Json::Value signals(Json::arrayValue);
    for (int id : session.visibleSignalIds) signals.append(id);
    root["signals"] = signals;

    Json::Value markers(Json::arrayValue);
    for (const WaveMarker& marker : session.markers) {
        Json::Value item(Json::objectValue);
        item["time"] = static_cast<Json::UInt64>(marker.time);
        item["label"] = marker.label;
        item["color"] = marker.color;
        markers.append(item);
    }
    root["markers"] = markers;

    Json::Value events(Json::arrayValue);
    for (const WaveEvent& event : session.events) {
        Json::Value item(Json::objectValue);
        item["time"] = static_cast<Json::UInt64>(event.time);
        item["label"] = event.label;
        item["color"] = event.color;
        item["signal_name"] = event.signalName;
        item["source_path"] = event.sourcePath;
        item["source_line"] = event.sourceLine;
        events.append(item);
    }
    root["events"] = events;

    root["playhead"] = static_cast<Json::UInt64>(session.playhead);
    root["has_playhead"] = session.hasPlayhead;
    root["ab_a"] = static_cast<Json::UInt64>(session.abA);
    root["ab_b"] = static_cast<Json::UInt64>(session.abB);
    root["has_ab"] = session.hasAB;
    root["theme"] = ThemeName(session.theme);
    root["uart_capture_path"] = session.uartCapturePath;
    Json::Value aliases(Json::objectValue);
    for (const auto& item : session.signalAliases) {
        aliases[std::to_string(item.first)] = item.second;
    }
    root["signal_aliases"] = aliases;
    Json::Value comments(Json::objectValue);
    for (const auto& item : session.signalComments) {
        comments[std::to_string(item.first)] = item.second;
    }
    root["signal_comments"] = comments;

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    const std::string content = Json::writeString(writer, root) + "\n";

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        error = "unable to create session: " + path;
        return false;
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.flush();
    if (!out) {
        error = "unable to write session: " + path;
        return false;
    }
    return true;
}

bool WaveSessionLoad(const std::string& path, WaveSessionData& session,
                     std::string& error)
{
    std::ifstream in(path);
    if (!in) {
        error = "unable to open session: " + path;
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string parseErrors;
    if (!reader->parse(content.data(), content.data() + content.size(), &root,
                       &parseErrors)) {
        error = "unable to parse session: " + parseErrors;
        return false;
    }

    session = WaveSessionData();
    session.sourcePath = root.get("source_path", "").asString();
    session.timeOffset = root.get("time_offset", 0).asUInt64();
    session.timeSpan = root.get("time_span", 1000).asUInt64();
    if (session.timeSpan < 1) session.timeSpan = 1;

    for (const Json::Value& item : root["signals"]) {
        session.visibleSignalIds.push_back(item.asInt());
    }
    for (const Json::Value& item : root["markers"]) {
        WaveMarker marker;
        marker.time = item.get("time", 0).asUInt64();
        marker.label = item.get("label", "").asString();
        marker.color = static_cast<std::uint32_t>(item.get("color", 0xFF3B82F6).asUInt());
        session.markers.push_back(marker);
    }
    for (const Json::Value& item : root["events"]) {
        WaveEvent event;
        event.time = item.get("time", 0).asUInt64();
        event.label = item.get("label", "").asString();
        event.color = static_cast<std::uint32_t>(item.get("color", 0xFFF59E0B).asUInt());
        event.signalName = item.get("signal_name", "").asString();
        event.sourcePath = item.get("source_path", "").asString();
        event.sourceLine = item.get("source_line", 0).asInt();
        session.events.push_back(event);
    }
    session.playhead = root.get("playhead", 0).asUInt64();
    session.hasPlayhead = root.get("has_playhead", false).asBool();
    session.abA = root.get("ab_a", 0).asUInt64();
    session.abB = root.get("ab_b", 0).asUInt64();
    session.hasAB = root.get("has_ab", false).asBool();
    session.theme = ThemeFromName(root.get("theme", "dark").asString());
    session.uartCapturePath = root.get("uart_capture_path", "").asString();
    for (const std::string& key : root["signal_aliases"].getMemberNames()) {
        session.signalAliases[std::stoi(key)] = root["signal_aliases"][key].asString();
    }
    for (const std::string& key : root["signal_comments"].getMemberNames()) {
        session.signalComments[std::stoi(key)] = root["signal_comments"][key].asString();
    }
    return true;
}

} // namespace wave
} // namespace sigflow
