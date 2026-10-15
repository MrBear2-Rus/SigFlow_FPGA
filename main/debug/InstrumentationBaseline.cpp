#include "InstrumentationBaseline.h"

#include "DebugArtifactIO.h"

#include <json/json.h>

#include <filesystem>
#include <fstream>

namespace sigflow::debug {
namespace {

Json::Value StringsJson(const std::vector<std::string>& values)
{
    Json::Value array(Json::arrayValue);
    for (const auto& value : values) array.append(value);
    return array;
}

void IdentityJson(Json::Value& target, const InstrumentationBuildIdentity& identity)
{
    target["source_fingerprint"] = identity.sourceFingerprint;
    target["top_module"] = identity.topModule;
    target["target_device"] = identity.targetDevice;
    target["speed_grade"] = identity.speedGrade;
    target["tool_version"] = identity.toolVersion;
    target["synthesis_strategy"] = identity.synthesisStrategy;
    target["constraints_fingerprint"] = identity.constraintsFingerprint;
    target["placement_seed"] = identity.placementSeed;
    target["clock_domains"] = StringsJson(identity.clockDomains);
    target["io_signature"] = StringsJson(identity.ioSignature);
}

bool ReadString(const Json::Value& object, const char* key, std::string& out,
                std::string& error, bool required = true)
{
    if (!object.isMember(key)) {
        if (required) error = std::string("baseline catalog missing field: ") + key;
        return !required;
    }
    if (!object[key].isString()) {
        error = std::string("baseline catalog field is not a string: ") + key;
        return false;
    }
    out = object[key].asString();
    return true;
}

bool ReadStrings(const Json::Value& object, const char* key,
                 std::vector<std::string>& out, std::string& error)
{
    if (!object.isMember(key) || !object[key].isArray()) {
        error = std::string("baseline catalog field is not an array: ") + key;
        return false;
    }
    out.clear();
    for (const auto& item : object[key]) {
        if (!item.isString()) {
            error = std::string("baseline catalog array contains a non-string: ") + key;
            return false;
        }
        out.push_back(item.asString());
    }
    return true;
}

bool ReadIdentity(const Json::Value& object, InstrumentationBuildIdentity& out,
                  std::string& error)
{
    return ReadString(object, "source_fingerprint", out.sourceFingerprint, error) &&
           ReadString(object, "top_module", out.topModule, error) &&
           ReadString(object, "target_device", out.targetDevice, error) &&
           ReadString(object, "speed_grade", out.speedGrade, error) &&
           ReadString(object, "tool_version", out.toolVersion, error) &&
           ReadString(object, "synthesis_strategy", out.synthesisStrategy, error) &&
           ReadString(object, "constraints_fingerprint", out.constraintsFingerprint, error) &&
           ReadString(object, "placement_seed", out.placementSeed, error) &&
           ReadStrings(object, "clock_domains", out.clockDomains, error) &&
           ReadStrings(object, "io_signature", out.ioSignature, error);
}

Json::Value RecordJson(const InstrumentationBaselineRecord& record)
{
    Json::Value item(Json::objectValue);
    item["baseline_id"] = record.baselineId;
    item["report_path"] = record.reportPath;
    item["source_fingerprint"] = record.sourceFingerprint;
    item["created_utc"] = record.createdUtc;
    IdentityJson(item["identity"], record.identity);
    return item;
}

bool ReadRecord(const Json::Value& item, InstrumentationBaselineRecord& out,
                std::string& error)
{
    if (!item.isObject() || !ReadString(item, "baseline_id", out.baselineId, error) ||
        !ReadString(item, "report_path", out.reportPath, error) ||
        !ReadString(item, "source_fingerprint", out.sourceFingerprint, error) ||
        !ReadString(item, "created_utc", out.createdUtc, error) ||
        !item.isMember("identity") || !ReadIdentity(item["identity"], out.identity, error)) {
        if (error.empty()) error = "baseline catalog record is invalid";
        return false;
    }
    if (out.baselineId.empty() || out.reportPath.empty() || out.sourceFingerprint.empty()) {
        error = "baseline catalog record has an empty identity or report path";
        return false;
    }
    if (out.identity.sourceFingerprint != out.sourceFingerprint) {
        error = "baseline catalog record source fingerprints differ";
        return false;
    }
    return true;
}

} // namespace

bool InstrumentationBaselineCatalog::Add(const InstrumentationBaselineRecord& record,
                                          std::string& error)
{
    error.clear();
    if (record.baselineId.empty() || record.reportPath.empty() || record.sourceFingerprint.empty()) {
        error = "baseline record requires baseline_id, report_path and source_fingerprint";
        return false;
    }
    if (record.identity.sourceFingerprint != record.sourceFingerprint) {
        error = "baseline record identity source fingerprint does not match";
        return false;
    }
    for (const auto& existing : records_) {
        if (existing.baselineId == record.baselineId) {
            error = "baseline id already exists; refusing to overwrite existing session";
            return false;
        }
    }
    records_.push_back(record);
    return true;
}

bool InstrumentationBaselineCatalog::Select(const std::string& baselineId,
                                            const std::string& currentSourceFingerprint,
                                            InstrumentationBaselineSelection& out,
                                            std::string& error) const
{
    error.clear();
    out = InstrumentationBaselineSelection();
    if (baselineId.empty() || currentSourceFingerprint.empty()) {
        error = "baseline selection requires baseline_id and current source fingerprint";
        return false;
    }
    for (const auto& record : records_) {
        if (record.baselineId != baselineId) continue;
        out.baselineId = record.baselineId;
        out.reportPath = record.reportPath;
        if (record.sourceFingerprint != currentSourceFingerprint) {
            out.invalidReason = "source fingerprint changed since baseline was created";
            out.valid = false;
        } else {
            out.valid = true;
        }
        return true;
    }
    error = "baseline id was not found";
    return false;
}

bool InstrumentationBaselineCatalog::Save(const std::string& path, std::string& error) const
{
    error.clear();
    if (path.empty()) {
        error = "baseline catalog path is empty";
        return false;
    }
    Json::Value root(Json::objectValue);
    root["schema_version"] = schemaVersion;
    root["record_count"] = Json::UInt(records_.size());
    Json::Value records(Json::arrayValue);
    for (const auto& record : records_) records.append(RecordJson(record));
    root["records"] = records;
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    return WriteTextFileAtomic(std::filesystem::path(path), Json::writeString(writer, root) + "\n", error);
}

bool InstrumentationBaselineCatalog::Load(const std::string& path, std::string& error)
{
    error.clear();
    if (path.empty()) {
        error = "baseline catalog path is empty";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open baseline catalog: " + path;
        return false;
    }
    Json::Value root;
    Json::CharReaderBuilder reader;
    if (!Json::parseFromStream(reader, input, &root, &error) || !root.isObject()) {
        if (error.empty()) error = "baseline catalog root is not an object";
        return false;
    }
    std::string version;
    if (!ReadString(root, "schema_version", version, error) || version != "1.0" ||
        !root.isMember("record_count") || !root["record_count"].isUInt() ||
        !root.isMember("records") || !root["records"].isArray()) {
        if (error.empty()) error = "baseline catalog header is invalid";
        return false;
    }
    if (root["record_count"].asUInt64() != root["records"].size()) {
        error = "baseline catalog record_count does not match records";
        return false;
    }
    std::vector<InstrumentationBaselineRecord> parsed;
    for (const auto& item : root["records"]) {
        InstrumentationBaselineRecord record;
        if (!ReadRecord(item, record, error)) return false;
        for (const auto& existing : parsed) {
            if (existing.baselineId == record.baselineId) {
                error = "baseline catalog contains duplicate baseline_id";
                return false;
            }
        }
        parsed.push_back(std::move(record));
    }
    schemaVersion = version;
    records_ = std::move(parsed);
    return true;
}

} // namespace sigflow::debug
