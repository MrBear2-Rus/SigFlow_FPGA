#include "DebugContract.h"

#include "DebugThresholds.h"

#include <json/json.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>

namespace sigflow {
namespace debug {

namespace {

bool IsValidIdentifier(const std::string& value)
{
    if (value.empty()) return false;
    const char first = value[0];
    if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
          first == '_')) {
        return false;
    }
    for (char c : value) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '$')) {
            return false;
        }
    }
    return true;
}

bool IsHex(const std::string& value)
{
    if (value.empty()) return false;
    std::size_t offset = (value.size() >= 2 && value[0] == '0' &&
                          (value[1] == 'x' || value[1] == 'X'))
                             ? 2
                             : 0;
    if (offset >= value.size()) return false;
    for (std::size_t i = offset; i < value.size(); ++i) {
        const char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string NewDebugSessionId()
{
    static std::atomic<unsigned long> sequence{0};
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d%02d%02dT%02d%02d%02d-%06lu",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec,
                  ++sequence);
    return std::string(buffer);
}

bool ParseHexU32(const std::string& text, std::uint32_t& value)
{
    if (text.empty()) return false;
    std::size_t offset = (text.size() >= 2 && text[0] == '0' &&
                          (text[1] == 'x' || text[1] == 'X'))
                             ? 2
                             : 0;
    if (offset >= text.size()) return false;
    std::uint32_t result = 0;
    for (std::size_t i = offset; i < text.size(); ++i) {
        const char c = text[i];
        unsigned digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned>(c - 'a') + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<unsigned>(c - 'A') + 10;
        } else {
            return false;
        }
        result = (result << 4) | digit;
    }
    value = result;
    return true;
}

void DebugContract::ApplyDefaults()
{
    if (capture.depth == 0) capture.depth = DebugDefaults::kCaptureDepth;
    if (capture.pretriggerSamples == 0) {
        capture.pretriggerSamples = DebugDefaults::kPretriggerSamples;
    }
    if (capture.decimation == 0) capture.decimation = 1;
    if (transport.baud == 0) transport.baud = DebugDefaults::kUartBaud;
    if (transport.kind.empty()) transport.kind = "uart";
    if (transport.protocol.empty()) transport.protocol = "minimal";
    if (trigger.kind.empty()) trigger.kind = "none";
}

bool DebugContract::ParseJson(const std::string& json, std::string& error)
{
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string parseErrors;
    if (!reader->parse(json.data(), json.data() + json.size(), &root, &parseErrors)) {
        error = "contract JSON parse error: " + parseErrors;
        return false;
    }
    if (!root.isObject()) {
        error = "contract root must be an object";
        return false;
    }

    *this = DebugContract();
    schemaVersion = root.get("schema_version", "1.0").asString();
    sessionId = root.get("session_id", "").asString();
    targetProfile = root.get("target_profile", "").asString();
    topModule = root.get("top_module", "").asString();

    const Json::Value& clock = root["sample_clock"];
    if (clock.isObject()) {
        sampleClock.signal = clock.get("signal", "").asString();
        sampleClock.frequencyHz = clock.get("frequency_hz", 0).asUInt64();
    }

    for (const Json::Value& item : root["probes"]) {
        if (!item.isObject()) continue;
        DebugProbe probe;
        probe.id = item.get("id", "").asString();
        probe.path = item.get("path", "").asString();
        probe.width = static_cast<unsigned>(item.get("width", 1).asUInt());
        probe.bitOffset = static_cast<unsigned>(item.get("bit_offset", 0).asUInt());
        probe.clockDomain = item.get("clock_domain", "").asString();
        probes.push_back(std::move(probe));
    }

    const Json::Value& trig = root["trigger"];
    if (trig.isObject()) {
        trigger.kind = trig.get("kind", "none").asString();
        trigger.mask = trig.get("mask", "").asString();
        trigger.value = trig.get("value", "").asString();
        trigger.intentKind = trig.get("intent", "").asString();
        if (trig["intent_params"].isString()) {
            trigger.intentParams = trig["intent_params"].asString();
        }
        trigger.expanded = trig.get("expanded", "").asString();
        trigger.hsValidPath = trig.get("hs_valid_path", "").asString();
        trigger.hsReadyPath = trig.get("hs_ready_path", "").asString();
    }

    const Json::Value& cap = root["capture"];
    if (cap.isObject()) {
        capture.depth = static_cast<std::uint32_t>(cap.get("depth", 0).asUInt());
        capture.pretriggerSamples =
            static_cast<std::uint32_t>(cap.get("pretrigger_samples", 0).asUInt());
        capture.decimation =
            static_cast<std::uint32_t>(cap.get("decimation", 1).asUInt());
    }

    const Json::Value& trans = root["transport"];
    if (trans.isObject()) {
        transport.kind = trans.get("kind", "uart").asString();
        transport.protocol = trans.get("protocol", "minimal").asString();
        transport.txPort = trans.get("tx_port", "").asString();
        transport.rxPort = trans.get("rx_port", "").asString();
        transport.baud = static_cast<std::uint32_t>(trans.get("baud", 0).asUInt());
        transport.syncEnabled = trans.get("sync_enabled", true).asBool();
        transport.txPin = trans.get("tx_pin", 0).asInt();
        transport.rxPin = trans.get("rx_pin", 0).asInt();
        transport.rstPin = trans.get("rst_pin", 0).asInt();
    }

    const Json::Value& fp = root["fingerprints"];
    if (fp.isObject()) {
        fingerprints.source = fp.get("source", "").asString();
        fingerprints.toolchain = fp.get("toolchain", "").asString();
        fingerprints.bitstream = fp.get("bitstream", "").asString();
        fingerprints.fingerprint64 =
            static_cast<std::uint64_t>(fp.get("fingerprint64", 0).asUInt64());
    }

    ApplyDefaults();
    return true;
}

std::string DebugContract::ToJson() const
{
    Json::Value root(Json::objectValue);
    root["schema_version"] = schemaVersion;
    root["session_id"] = sessionId;
    root["target_profile"] = targetProfile;
    root["top_module"] = topModule;

    Json::Value clock(Json::objectValue);
    clock["signal"] = sampleClock.signal;
    clock["frequency_hz"] = static_cast<Json::UInt64>(sampleClock.frequencyHz);
    root["sample_clock"] = clock;

    Json::Value probesArray(Json::arrayValue);
    for (const DebugProbe& probe : probes) {
        Json::Value item(Json::objectValue);
        item["id"] = probe.id;
        item["path"] = probe.path;
        item["width"] = probe.width;
        item["bit_offset"] = probe.bitOffset;
        item["clock_domain"] = probe.clockDomain;
        probesArray.append(item);
    }
    root["probes"] = probesArray;

    Json::Value trig(Json::objectValue);
    trig["kind"] = trigger.kind;
    trig["mask"] = trigger.mask;
    trig["value"] = trigger.value;
    trig["intent"] = trigger.intentKind;
    trig["intent_params"] = trigger.intentParams;
    trig["expanded"] = trigger.expanded;
    trig["hs_valid_path"] = trigger.hsValidPath;
    trig["hs_ready_path"] = trigger.hsReadyPath;
    root["trigger"] = trig;

    Json::Value cap(Json::objectValue);
    cap["depth"] = capture.depth;
    cap["pretrigger_samples"] = capture.pretriggerSamples;
    cap["decimation"] = capture.decimation;
    root["capture"] = cap;

    Json::Value trans(Json::objectValue);
    trans["kind"] = transport.kind;
    trans["protocol"] = transport.protocol;
    trans["tx_port"] = transport.txPort;
    trans["rx_port"] = transport.rxPort;
    trans["baud"] = transport.baud;
    trans["sync_enabled"] = transport.syncEnabled;
    trans["tx_pin"] = transport.txPin;
    trans["rx_pin"] = transport.rxPin;
    trans["rst_pin"] = transport.rstPin;
    root["transport"] = trans;

    Json::Value fp(Json::objectValue);
    fp["source"] = fingerprints.source;
    fp["toolchain"] = fingerprints.toolchain;
    fp["bitstream"] = fingerprints.bitstream;
    fp["fingerprint64"] = static_cast<Json::UInt64>(fingerprints.fingerprint64);
    root["fingerprints"] = fp;

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    return Json::writeString(writer, root) + "\n";
}

bool DebugContract::AssignProbeBitOffsets(std::string& error)
{
    unsigned offset = 0;
    for (DebugProbe& probe : probes) {
        probe.bitOffset = offset;
        offset += probe.width;
        if (offset > DebugDefaults::kProbeTotalBits) {
            error = "total probe width exceeds " +
                    std::to_string(DebugDefaults::kProbeTotalBits) + " bits";
            return false;
        }
    }
    return true;
}

bool DebugContract::Validate(std::string& error) const
{
    if (schemaVersion != "1.0") {
        error = "unsupported contract schema_version: " + schemaVersion;
        return false;
    }
    if (sessionId.empty()) {
        error = "contract requires session_id";
        return false;
    }
    if (topModule.empty() || !IsValidIdentifier(topModule)) {
        error = "contract requires a valid top_module identifier";
        return false;
    }
    if (sampleClock.signal.empty() || sampleClock.frequencyHz == 0) {
        error = "contract requires sample_clock.signal and frequency_hz";
        return false;
    }
    if (probes.empty()) {
        error = "contract requires at least one probe";
        return false;
    }
    unsigned totalBits = 0;
    for (const DebugProbe& probe : probes) {
        if (probe.id.empty() || probe.path.empty()) {
            error = "each probe requires id and path";
            return false;
        }
        if (probe.width == 0) {
            error = "probe width must be positive: " + probe.id;
            return false;
        }
        totalBits += probe.width;
    }
    if (totalBits > DebugDefaults::kProbeTotalBits) {
        error = "total probe width exceeds " +
                std::to_string(DebugDefaults::kProbeTotalBits) + " bits";
        return false;
    }
    if (capture.depth < 1) {
        error = "capture.depth must be positive";
        return false;
    }
    if (capture.pretriggerSamples > capture.depth) {
        error = "capture.pretrigger_samples must not exceed depth";
        return false;
    }
    if (capture.decimation < 1) {
        error = "capture.decimation must be positive";
        return false;
    }
    if (transport.kind != "uart") {
        error = "unsupported transport kind: " + transport.kind;
        return false;
    }
    if (transport.protocol != "minimal" && transport.protocol != "full") {
        error = "unsupported UART debug protocol: " + transport.protocol;
        return false;
    }
    if (transport.baud != 115200 && transport.baud != 460800 &&
        transport.baud != 921600 && transport.baud != 1500000 &&
        transport.baud != 3000000) {
        error = "unsupported transport baud: " + std::to_string(transport.baud);
        return false;
    }
    if (transport.txPort.empty() || transport.rxPort.empty()) {
        error = "transport requires tx_port and rx_port";
        return false;
    }
    if (trigger.kind != "none" && trigger.kind != "mask_equal" &&
        trigger.kind != "edge_rising" && trigger.kind != "edge_falling" &&
        trigger.kind != "comb_and") {
        error = "unsupported trigger kind: " + trigger.kind;
        return false;
    }
    if (trigger.kind == "mask_equal" &&
        (!IsHex(trigger.mask) || !IsHex(trigger.value))) {
        error = "mask_equal trigger requires hex mask and value";
        return false;
    }
    return true;
}

} // namespace debug
} // namespace sigflow
