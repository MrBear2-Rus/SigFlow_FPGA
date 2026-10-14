#include "WaveProtocolEventReport.h"

#include "../debug/DebugArtifactIO.h"
#include "../trace/TraceTimebase.h"
#include <json/json.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace sigflow::wave {
namespace {

const char* TimeDomainName(sigflow::trace::TraceTimeDomain domain)
{
    switch (domain) {
        case sigflow::trace::TraceTimeDomain::Physical: return "physical";
        case sigflow::trace::TraceTimeDomain::SampleIndex: return "sample_index";
        default: return "unknown";
    }
}

Json::Value TransactionJson(const ProtocolTransaction& transaction)
{
    Json::Value item(Json::objectValue);
    item["protocol"] = transaction.protocol;
    item["start_time"] = Json::UInt64(transaction.startTime);
    item["end_time"] = Json::UInt64(transaction.endTime);
    item["valid"] = transaction.valid;
    item["wait_cycles"] = Json::UInt64(transaction.waitCycles);
    item["detail"] = transaction.detail;
    Json::Value bytes(Json::arrayValue);
    for (const std::uint8_t byte : transaction.bytes) bytes.append(Json::UInt(byte));
    item["bytes"] = bytes;
    Json::Value words(Json::arrayValue);
    for (const std::uint64_t word : transaction.words) words.append(Json::UInt64(word));
    item["words"] = words;
    return item;
}

std::string CsvField(const std::string& value)
{
    std::string escaped = "\"";
    for (const char c : value) {
        if (c == '\"') escaped += "\"\"";
        else escaped += c;
    }
    escaped += '\"';
    return escaped;
}

std::string NumberList(const std::vector<std::uint64_t>& values)
{
    std::ostringstream output;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) output << ';';
        output << values[i];
    }
    return output.str();
}

std::string ByteList(const std::vector<std::uint8_t>& values)
{
    std::vector<std::uint64_t> promoted;
    promoted.reserve(values.size());
    for (const std::uint8_t value : values) promoted.push_back(value);
    return NumberList(promoted);
}

bool ReadRequiredString(const Json::Value& object, const char* key,
                        std::string& value, std::string& error)
{
    if (!object.isMember(key) || !object[key].isString()) {
        error = std::string("protocol event report field is not a string: ") + key;
        return false;
    }
    value = object[key].asString();
    return true;
}

bool ReadTransaction(const Json::Value& item, ProtocolTransaction& out, std::string& error)
{
    if (!item.isObject()) {
        error = "protocol event report event is not an object";
        return false;
    }
    if (!ReadRequiredString(item, "protocol", out.protocol, error) || out.protocol.empty()) {
        if (error.empty()) error = "protocol event report event has empty protocol";
        return false;
    }
    const char* integerFields[] = {"start_time", "end_time", "wait_cycles"};
    for (const char* key : integerFields) {
        if (!item.isMember(key) || !item[key].isUInt64()) {
            error = std::string("protocol event report field is not an unsigned integer: ") + key;
            return false;
        }
    }
    if (!item.isMember("valid") || !item["valid"].isBool() ||
        !item.isMember("detail") || !item["detail"].isString()) {
        error = "protocol event report event has invalid validity/detail fields";
        return false;
    }
    out.startTime = item["start_time"].asUInt64();
    out.endTime = item["end_time"].asUInt64();
    out.waitCycles = item["wait_cycles"].asUInt64();
    if (out.endTime < out.startTime) {
        error = "protocol event report event ends before it starts";
        return false;
    }
    out.valid = item["valid"].asBool();
    out.detail = item["detail"].asString();
    if (!item.isMember("bytes") || !item["bytes"].isArray() ||
        !item.isMember("words") || !item["words"].isArray()) {
        error = "protocol event report event has invalid byte/word arrays";
        return false;
    }
    for (const Json::Value& byte : item["bytes"]) {
        if (!byte.isUInt() || byte.asUInt() > 0xFFu) {
            error = "protocol event report byte is outside 0..255";
            return false;
        }
        out.bytes.push_back(static_cast<std::uint8_t>(byte.asUInt()));
    }
    for (const Json::Value& word : item["words"]) {
        if (!word.isUInt64()) {
            error = "protocol event report word is not an unsigned integer";
            return false;
        }
        out.words.push_back(word.asUInt64());
    }
    return true;
}

} // namespace

bool BuildProtocolEventReport(const sigflow::trace::TraceSource& source,
                              const std::string& captureId,
                              const std::string& evidencePath,
                              const std::string& configurationHash,
                              const std::vector<ProtocolTransaction>& transactions,
                              ProtocolEventReport& out,
                              std::string& error)
{
    error.clear();
    out = ProtocolEventReport();
    if (captureId.empty()) {
        error = "protocol event report requires capture_id";
        return false;
    }
    const auto range = source.TimeRange();
    if (!range.valid || range.end < range.begin) {
        error = "protocol event report requires a valid source time range";
        return false;
    }
    for (const ProtocolTransaction& transaction : transactions) {
        if (transaction.protocol.empty()) {
            error = "protocol event report contains an event without protocol";
            return false;
        }
        if (transaction.endTime < transaction.startTime ||
            transaction.startTime < range.begin || transaction.endTime > range.end) {
            error = "protocol event is outside the source time range";
            return false;
        }
    }
    out.captureId = captureId;
    out.sourcePath = source.Path();
    out.timeDomain = TimeDomainName(source.TimeDomain());
    out.timescale = source.Timescale();
    out.evidencePath = evidencePath;
    out.configurationHash = configurationHash;
    out.transactions = transactions;
    std::stable_sort(out.transactions.begin(), out.transactions.end(),
                     [](const ProtocolTransaction& left, const ProtocolTransaction& right) {
                         if (left.startTime != right.startTime) return left.startTime < right.startTime;
                         if (left.endTime != right.endTime) return left.endTime < right.endTime;
                         return left.protocol < right.protocol;
                     });
    if (out.timeDomain == "unknown") {
        out.diagnostics.push_back("TIME_DOMAIN_UNKNOWN: source time domain is not verified");
    }
    return true;
}

std::string GenerateProtocolEventReportJson(const ProtocolEventReport& report)
{
    Json::Value root(Json::objectValue);
    root["schema_version"] = report.schemaVersion;
    root["decoder_version"] = report.decoderVersion;
    root["capture_id"] = report.captureId;
    root["source_path"] = report.sourcePath;
    root["time_domain"] = report.timeDomain;
    root["timescale"] = report.timescale;
    root["evidence_path"] = report.evidencePath;
    root["configuration_hash"] = report.configurationHash;
    root["event_count"] = Json::UInt(report.transactions.size());
    Json::Value events(Json::arrayValue);
    for (const ProtocolTransaction& transaction : report.transactions) {
        events.append(TransactionJson(transaction));
    }
    root["events"] = events;
    Json::Value diagnostics(Json::arrayValue);
    for (const std::string& diagnostic : report.diagnostics) diagnostics.append(diagnostic);
    root["diagnostics"] = diagnostics;
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    return Json::writeString(writer, root) + "\n";
}

std::string GenerateProtocolEventReportCsv(const ProtocolEventReport& report)
{
    std::ostringstream output;
    output << "capture_id,protocol,start_time,end_time,valid,wait_cycles,detail,bytes,words\n";
    for (const ProtocolTransaction& transaction : report.transactions) {
        output << CsvField(report.captureId) << ','
                << CsvField(transaction.protocol) << ','
                << transaction.startTime << ','
                << transaction.endTime << ','
                << (transaction.valid ? "true" : "false") << ','
                << transaction.waitCycles << ','
                << CsvField(transaction.detail) << ','
                << CsvField(ByteList(transaction.bytes)) << ','
                << CsvField(NumberList(transaction.words)) << '\n';
    }
    return output.str();
}

bool SaveProtocolEventReport(const ProtocolEventReport& report,
                             const std::string& path,
                             std::string& error)
{
    if (path.empty()) {
        error = "protocol event report path is empty";
        return false;
    }
    return sigflow::debug::WriteTextFileAtomic(
        std::filesystem::path(path), GenerateProtocolEventReportJson(report), error);
}

bool SaveProtocolEventReportCsv(const ProtocolEventReport& report,
                                const std::string& path,
                                std::string& error)
{
    if (path.empty()) {
        error = "protocol event CSV report path is empty";
        return false;
    }
    return sigflow::debug::WriteTextFileAtomic(
        std::filesystem::path(path), GenerateProtocolEventReportCsv(report), error);
}

bool LoadProtocolEventReport(const std::string& path,
                             ProtocolEventReport& out,
                             std::string& error)
{
    error.clear();
    out = ProtocolEventReport();
    if (path.empty()) {
        error = "protocol event report path is empty";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open protocol event report: " + path;
        return false;
    }
    Json::Value root;
    Json::CharReaderBuilder reader;
    if (!Json::parseFromStream(reader, input, &root, &error) || !root.isObject()) {
        if (error.empty()) error = "protocol event report root is not an object";
        return false;
    }
    if (!ReadRequiredString(root, "schema_version", out.schemaVersion, error) ||
        out.schemaVersion != "1.0" ||
        !ReadRequiredString(root, "decoder_version", out.decoderVersion, error) ||
        !ReadRequiredString(root, "capture_id", out.captureId, error) ||
        out.captureId.empty() ||
        !ReadRequiredString(root, "source_path", out.sourcePath, error) ||
        !ReadRequiredString(root, "time_domain", out.timeDomain, error) ||
        !ReadRequiredString(root, "timescale", out.timescale, error) ||
        !ReadRequiredString(root, "evidence_path", out.evidencePath, error) ||
        !ReadRequiredString(root, "configuration_hash", out.configurationHash, error)) {
        if (error.empty()) error = "protocol event report header is invalid";
        return false;
    }
    if (out.timeDomain != "physical" && out.timeDomain != "sample_index" &&
        out.timeDomain != "unknown") {
        error = "protocol event report time_domain is unsupported";
        return false;
    }
    if (!root.isMember("event_count") || !root["event_count"].isUInt() ||
        !root.isMember("events") || !root["events"].isArray() ||
        !root.isMember("diagnostics") || !root["diagnostics"].isArray()) {
        error = "protocol event report event envelope is invalid";
        return false;
    }
    if (root["event_count"].asUInt64() != root["events"].size()) {
        error = "protocol event report event_count does not match events";
        return false;
    }
    for (const Json::Value& diagnostic : root["diagnostics"]) {
        if (!diagnostic.isString()) {
            error = "protocol event report diagnostic is not a string";
            return false;
        }
        out.diagnostics.push_back(diagnostic.asString());
    }
    for (const Json::Value& item : root["events"]) {
        ProtocolTransaction transaction;
        if (!ReadTransaction(item, transaction, error)) return false;
        out.transactions.push_back(std::move(transaction));
    }
    return true;
}

} // namespace sigflow::wave
