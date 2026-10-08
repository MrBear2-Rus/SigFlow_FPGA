#include "SnapshotService.h"

#include "eda-platform/Platform.h"
#include "eda-platform/Sha256.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

namespace eda {
namespace agent {
namespace {

struct PreparedSource {
    SnapshotSource record;
    std::filesystem::path absolutePath;
    std::string content;
};

std::uint64_t NowEpoch() { return static_cast<std::uint64_t>(std::time(nullptr)); }

// 目录提交：Windows 上刚写完文件的目录 rename 可能被防病毒/搜索索引器短暂持有
// （ERROR_SHARING_VIOLATION / ERROR_ACCESS_DENIED），这是**可恢复**的瞬时错误。
// 不做重试就会把一次正常提交报成 503，学生看到"快照提交失败"但重试一次就好了。
// 这里做有界退避重试（总计约 150 ms），失败原因仍然如实上报。
bool RenameDirectoryWithRetry(const std::filesystem::path& from,
                              const std::filesystem::path& to, std::error_code& error) {
    constexpr int kAttempts = 6;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        error.clear();
        std::filesystem::rename(from, to, error);
        if (!error) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10 * (attempt + 1)));
    }
    return false;
}

SnapshotResult Fail(SnapshotError code, std::string message) {
    SnapshotResult result;
    result.code = code;
    result.message = std::move(message);
    return result;
}

void StableDump(const Json& value, std::ostringstream& out) {
    if (value.is_object()) {
        std::vector<std::string> keys;
        keys.reserve(value.size());
        for (auto it = value.begin(); it != value.end(); ++it) keys.push_back(it.key());
        std::sort(keys.begin(), keys.end());
        out << '{';
        bool first = true;
        for (const auto& key : keys) {
            if (!first) out << ',';
            first = false;
            out << Json(key).dump() << ':';
            StableDump(value.at(key), out);
        }
        out << '}';
        return;
    }
    if (value.is_array()) {
        out << '[';
        bool first = true;
        for (const auto& item : value) {
            if (!first) out << ',';
            first = false;
            StableDump(item, out);
        }
        out << ']';
        return;
    }
    out << value.dump();
}

std::string StableHash(const Json& value) {
    std::ostringstream stream;
    StableDump(value, stream);
    const std::string serialized = stream.str();
    return platform::Sha256Hex(serialized.data(), serialized.size());
}

bool ReadFile(const std::filesystem::path& path, std::string& content) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    content.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return static_cast<bool>(input) || input.eof();
}

bool WriteFile(const std::filesystem::path& path, const std::string& content,
               std::string& error) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            error = "unable to create snapshot directory: " + ec.message();
            return false;
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "unable to write snapshot file";
        return false;
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.flush();
    if (!output) {
        error = "unable to flush snapshot file";
        return false;
    }
    return true;
}

bool IsSafeId(const std::string& value) {
    if (value.empty()) return false;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            continue;
        }
        return false;
    }
    return true;
}

bool IsSha256(const std::string& value) {
    if (value.size() != 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool ManifestMatches(const SnapshotRequest& request,
                     const std::filesystem::path& canonicalRoot) {
    if (request.expectedProjectManifestSha256.empty()) return true;
    return platform::Sha256FileHex(canonicalRoot / "sigflow.project") ==
           request.expectedProjectManifestSha256;
}

bool NormalizeRelativePath(const std::filesystem::path& input,
                           std::filesystem::path& normalized) {
    if (input.empty() || input.is_absolute() || input.has_root_name() || input.has_root_directory()) {
        return false;
    }
    normalized = input.lexically_normal();
    if (normalized.empty() || normalized == ".") return false;
    for (const auto& component : normalized) {
        if (component == ".." || component == "." || component.empty()) return false;
    }
    return true;
}

bool IsWithin(const std::filesystem::path& child, const std::filesystem::path& root) {
    auto childIt = child.begin();
    for (auto rootIt = root.begin(); rootIt != root.end(); ++rootIt, ++childIt) {
        if (childIt == child.end() || *childIt != *rootIt) return false;
    }
    return true;
}

Json FingerprintDocument(const SnapshotRequest& request,
                         const std::vector<PreparedSource>& sources) {
    Json sourceList = Json::array();
    for (const auto& source : sources) {
        sourceList.push_back(Json{{"source_id", source.record.sourceId},
                                  {"path", source.record.relativePath},
                                  {"sha256", source.record.sha256},
                                  {"size", std::to_string(source.record.size)},
                                  {"origin", source.record.origin}});
    }
    return Json{{"schema_version", "edu.snapshot.input.v1"},
                {"project_id", request.projectId},
                {"revision", request.currentRevision},
                {"top", request.top},
                {"sources", sourceList},
                {"target", request.target},
                {"tool_config", request.toolConfig}};
}

std::string NewSnapshotId(const std::string& fingerprint) {
    // 内容寻址：同 input_fingerprint 必得同 id。这使"同输入重复建快照"天然幂等，
    // 也避免并发提交在 rename 处互相覆盖/失败（见 Commit 的已存在分支）。
    return "snap-" + platform::Sha256Hex(fingerprint.data(), fingerprint.size()).substr(0, 24);
}

bool ParseRecord(const Json& document, SnapshotRecord& out, std::string& error) {
    if (!document.is_object() ||
        document.value("schema_version", std::string()) != "edu.snapshot.v1") {
        error = "unsupported snapshot manifest";
        return false;
    }
    SnapshotRecord parsed;
    parsed.id = document.value("snapshot_id", std::string());
    parsed.projectId = document.value("project_id", std::string());
    parsed.revision = document.value("revision", std::string());
    parsed.createdAt = document.value("created_at", std::string());
    try {
        parsed.createdAtEpoch =
            std::stoull(document.value("created_at_epoch", std::string("0")));
    } catch (const std::exception&) {
        error = "invalid snapshot creation epoch";
        return false;
    }
    parsed.inputFingerprint = document.value("input_fingerprint", std::string());
    parsed.top = document.value("top", std::string());
    parsed.target = document.value("target", Json::object());
    parsed.toolConfig = document.value("tool_config", Json::object());
    if (parsed.id.empty() || parsed.projectId.empty() || parsed.revision.empty() ||
        parsed.inputFingerprint.size() != 64 || !document.contains("sources") ||
        !document["sources"].is_array()) {
        error = "invalid snapshot manifest";
        return false;
    }
    for (const auto& item : document["sources"]) {
        if (!item.is_object()) {
            error = "invalid snapshot source entry";
            return false;
        }
        SnapshotSource source;
        source.sourceId = item.value("source_id", std::string());
        source.relativePath = item.value("path", std::string());
        source.sha256 = item.value("sha256", std::string());
        source.origin = item.value("origin", std::string("project"));
        try {
            const std::string size = item.value("size", std::string());
            source.size = std::stoull(size);
        } catch (const std::exception&) {
            error = "invalid snapshot source size";
            return false;
        }
        if (source.sourceId.empty() || source.relativePath.empty() || source.sha256.size() != 64 ||
            (source.origin != "project" && source.origin != "external")) {
            error = "invalid snapshot source entry";
            return false;
        }
        parsed.sources.push_back(std::move(source));
    }
    out = std::move(parsed);
    return true;
}

} // namespace

SnapshotService::SnapshotService(std::filesystem::path storageRoot)
    : SnapshotService(std::move(storageRoot), Limits{}) {}

SnapshotService::SnapshotService(std::filesystem::path storageRoot, Limits limits)
    : storageRoot_(std::move(storageRoot)), limits_(limits) {}

SnapshotResult SnapshotService::Create(const SnapshotRequest& request,
                                       const std::function<std::string()>& revisionProbe) {
    if (request.projectId.empty() || request.projectRoot.empty() ||
        request.expectedRevision.empty() || request.currentRevision.empty() ||
        storageRoot_.empty()) {
        return Fail(SnapshotError::InvalidArgument,
                    "project_id, project_root and revision are required");
    }
    if (request.expectedRevision != request.currentRevision) {
        return Fail(SnapshotError::StaleRevision, "expected revision does not match current revision");
    }
    if (request.dirty) {
        return Fail(SnapshotError::DirtyProject, "project has unsaved editor changes");
    }
    if (!request.synchronized) {
        return Fail(SnapshotError::SyncInProgress, "graph/code synchronization is not complete");
    }
    if (request.sources.empty() || request.sources.size() > limits_.maxSources) {
        return Fail(SnapshotError::InvalidArgument, "source count is empty or exceeds the limit");
    }
    if (!request.target.is_object() || !request.toolConfig.is_object()) {
        return Fail(SnapshotError::InvalidArgument, "target and tool_config must be objects");
    }
    if ((!request.expectedProjectManifestSha256.empty() &&
         !IsSha256(request.expectedProjectManifestSha256)) ||
        std::any_of(request.sources.begin(), request.sources.end(),
                    [](const SnapshotSourceInput& source) {
                        return !source.expectedSha256.empty() &&
                               !IsSha256(source.expectedSha256);
                    })) {
        return Fail(SnapshotError::InvalidArgument, "expected input hash is invalid");
    }
    if (revisionProbe && revisionProbe() != request.expectedRevision) {
        return Fail(SnapshotError::StaleRevision, "revision changed before snapshot collection");
    }

    std::error_code ec;
    const std::filesystem::path canonicalRoot =
        std::filesystem::weakly_canonical(request.projectRoot, ec);
    if (ec || canonicalRoot.empty()) {
        return Fail(SnapshotError::SourceUnavailable, "project root is unavailable");
    }
    if (!ManifestMatches(request, canonicalRoot)) {
        return Fail(SnapshotError::SourceChanged,
                    "project manifest changed after the revision was observed");
    }

    std::vector<std::filesystem::path> externalRoots;
    externalRoots.reserve(request.allowedExternalRoots.size());
    for (const auto& root : request.allowedExternalRoots) {
        if (!root.is_absolute()) {
            return Fail(SnapshotError::InvalidArgument,
                        "allowed external roots must be absolute paths");
        }
        const std::filesystem::path canonicalExternal =
            std::filesystem::weakly_canonical(root, ec);
        if (ec || canonicalExternal.empty() ||
            !std::filesystem::is_directory(canonicalExternal, ec) || ec) {
            return Fail(SnapshotError::SourceUnavailable,
                        "an allowed external root is unavailable");
        }
        externalRoots.push_back(canonicalExternal);
    }

    std::set<std::string> sourceIds;
    std::set<std::string> sourcePaths;
    std::vector<PreparedSource> prepared;
    prepared.reserve(request.sources.size());
    std::uint64_t totalBytes = 0;
    for (const auto& input : request.sources) {
        if (!IsSafeId(input.sourceId) || !sourceIds.insert(input.sourceId).second) {
            return Fail(SnapshotError::InvalidArgument, "source_id is invalid or duplicated");
        }
        std::filesystem::path relative;
        std::filesystem::path absolute;
        std::string origin = "project";
        if (!input.externalPath.empty()) {
            if (!input.externalPath.is_absolute()) {
                return Fail(SnapshotError::PathOutsideProject,
                            "external source path must be absolute");
            }
            absolute = std::filesystem::weakly_canonical(input.externalPath, ec);
            bool allowed = !ec;
            if (allowed) {
                allowed = std::any_of(externalRoots.begin(), externalRoots.end(),
                                      [&absolute](const std::filesystem::path& root) {
                                          return IsWithin(absolute, root);
                                      });
            }
            if (!allowed) {
                return Fail(SnapshotError::PathOutsideProject,
                            "external source is outside the allowed include roots");
            }
            relative = std::filesystem::path("external") / input.sourceId /
                       absolute.filename();
            origin = "external";
        } else {
            if (!NormalizeRelativePath(input.relativePath, relative)) {
                return Fail(SnapshotError::PathOutsideProject,
                            "source path must be a normalized project-relative path");
            }
            absolute = std::filesystem::weakly_canonical(canonicalRoot / relative, ec);
            if (ec || !IsWithin(absolute, canonicalRoot)) {
                return Fail(SnapshotError::PathOutsideProject,
                            "source resolves outside the project root");
            }
        }
        if (!NormalizeRelativePath(relative, relative)) {
            return Fail(SnapshotError::PathOutsideProject, "snapshot source path is invalid");
        }
        const std::string genericPath = platform::RelativePathToUtf8(relative);
        if (!sourcePaths.insert(genericPath).second) {
            return Fail(SnapshotError::InvalidArgument, "source path is duplicated");
        }
        if (!std::filesystem::is_regular_file(absolute, ec) || ec) {
            return Fail(SnapshotError::SourceUnavailable, "registered source is unavailable");
        }

        PreparedSource source;
        source.absolutePath = absolute;
        source.record.sourceId = input.sourceId;
        source.record.relativePath = genericPath;
        source.record.origin = origin;
        if (!ReadFile(absolute, source.content)) {
            return Fail(SnapshotError::SourceUnavailable, "unable to read registered source");
        }
        source.record.size = static_cast<std::uint64_t>(source.content.size());
        if (source.record.size > limits_.maxTotalBytes - totalBytes) {
            return Fail(SnapshotError::InvalidArgument, "snapshot source bytes exceed the limit");
        }
        totalBytes += source.record.size;
        source.record.sha256 = platform::Sha256Hex(source.content.data(), source.content.size());
        if (!input.expectedSha256.empty() &&
            input.expectedSha256 != source.record.sha256) {
            return Fail(SnapshotError::SourceChanged,
                        "source changed after the revision was observed");
        }
        prepared.push_back(std::move(source));
    }
    std::sort(prepared.begin(), prepared.end(), [](const PreparedSource& left,
                                                    const PreparedSource& right) {
        if (left.record.relativePath != right.record.relativePath) {
            return left.record.relativePath < right.record.relativePath;
        }
        return left.record.sourceId < right.record.sourceId;
    });

    // 文件读取完成后再核对一次，拒绝构造期间被修改的源文件。
    for (const auto& source : prepared) {
        const std::string currentHash = platform::Sha256FileHex(source.absolutePath);
        if (currentHash.empty() || currentHash != source.record.sha256) {
            return Fail(SnapshotError::SourceChanged, "source changed while snapshot was created");
        }
    }
    if (revisionProbe && revisionProbe() != request.expectedRevision) {
        return Fail(SnapshotError::StaleRevision, "revision changed while snapshot was created");
    }
    if (!ManifestMatches(request, canonicalRoot)) {
        return Fail(SnapshotError::SourceChanged,
                    "project manifest changed while snapshot was created");
    }

    const Json fingerprintDocument = FingerprintDocument(request, prepared);
    SnapshotRecord record;
    record.id = NewSnapshotId(StableHash(fingerprintDocument));
    record.projectId = request.projectId;
    record.revision = request.currentRevision;
    record.createdAt = platform::UtcTimestamp();
    record.createdAtEpoch = NowEpoch();
    record.inputFingerprint = StableHash(fingerprintDocument);
    record.top = request.top;
    record.target = request.target;
    record.toolConfig = request.toolConfig;
    for (const auto& source : prepared) record.sources.push_back(source.record);

    const std::filesystem::path finalDirectory = storageRoot_ / record.id;
    const std::filesystem::path temporaryDirectory = storageRoot_ / ("." + record.id + ".tmp");
    std::filesystem::create_directories(storageRoot_, ec);
    if (ec) return Fail(SnapshotError::StorageError, "unable to create snapshot storage");
    std::filesystem::remove_all(temporaryDirectory, ec);
    ec.clear();
    std::filesystem::create_directories(temporaryDirectory / "files", ec);
    if (ec) return Fail(SnapshotError::StorageError, "unable to create snapshot staging area");

    std::string writeError;
    for (const auto& source : prepared) {
        if (!WriteFile(temporaryDirectory / "files" / source.record.relativePath,
                       source.content, writeError)) {
            std::filesystem::remove_all(temporaryDirectory, ec);
            return Fail(SnapshotError::StorageError, writeError);
        }
    }
    if (!WriteFile(temporaryDirectory / "manifest.json", ToJson(record).dump(2) + "\n",
                   writeError)) {
        std::filesystem::remove_all(temporaryDirectory, ec);
        return Fail(SnapshotError::StorageError, writeError);
    }
    if (revisionProbe && revisionProbe() != request.expectedRevision) {
        std::filesystem::remove_all(temporaryDirectory, ec);
        return Fail(SnapshotError::StaleRevision, "revision changed before snapshot commit");
    }
    if (!ManifestMatches(request, canonicalRoot)) {
        std::filesystem::remove_all(temporaryDirectory, ec);
        return Fail(SnapshotError::SourceChanged,
                    "project manifest changed before snapshot commit");
    }
    if (!RenameDirectoryWithRetry(temporaryDirectory, finalDirectory, ec)) {
        // 目录名 = SHA-256(input_fingerprint) 前 24 位，因此 finalDirectory 存在等价于
        // "同输入的快照已存在"，其内容与本次完全一致（不可变、内容寻址）。此时视为成功；
        // 同时把已存在 manifest 的 created_at/epoch 保留为原值，避免"读旧写新"覆盖历史。
        SnapshotRecord existingRecord;
        std::string existingError;
        std::error_code existsError;
        if (std::filesystem::is_directory(finalDirectory, existsError) && !existsError &&
            Lookup(record.id, existingRecord, existingError)) {
            std::filesystem::remove_all(temporaryDirectory, ec);
            SnapshotResult existing;
            existing.snapshot = std::move(existingRecord);
            return existing;
        }
        // 目录存在但 manifest 不完整/损坏（例如上次提交中途失败）：本次已完整构建，
        // 用它替换该目录，使同输入的重试能够自愈，而不是永久失败。
        std::error_code replaceError;
        std::filesystem::remove_all(finalDirectory, replaceError);
        if (replaceError) {
            std::filesystem::remove_all(temporaryDirectory, ec);
            return Fail(SnapshotError::StorageError, "unable to commit snapshot directory");
        }
        if (RenameDirectoryWithRetry(temporaryDirectory, finalDirectory, replaceError)) {
            SnapshotResult replaced;
            replaced.snapshot = std::move(record);
            return replaced;
        }
        std::filesystem::remove_all(temporaryDirectory, ec);
        return Fail(SnapshotError::StorageError, "unable to commit snapshot directory");
    }

    SnapshotResult result;
    result.snapshot = std::move(record);
    return result;
}

bool SnapshotService::Lookup(const std::string& snapshotId, SnapshotRecord& out,
                             std::string& error) const {
    error.clear();
    if (!IsSafeId(snapshotId) || snapshotId.rfind("snap-", 0) != 0) {
        error = "invalid snapshot id";
        return false;
    }
    std::ifstream input(SnapshotDirectory(snapshotId) / "manifest.json", std::ios::binary);
    if (!input) {
        error = "snapshot not found";
        return false;
    }
    Json document;
    try {
        input >> document;
    } catch (const std::exception&) {
        error = "invalid snapshot manifest";
        return false;
    }
    if (!ParseRecord(document, out, error)) return false;
    if (out.id != snapshotId) {
        error = "snapshot manifest id mismatch";
        return false;
    }
    std::error_code ec;
    const std::filesystem::path fileRoot =
        std::filesystem::weakly_canonical(SnapshotDirectory(snapshotId) / "files", ec);
    if (ec || fileRoot.empty()) {
        error = "snapshot files are unavailable";
        return false;
    }
    for (const auto& source : out.sources) {
        std::filesystem::path relative;
        if (!NormalizeRelativePath(source.relativePath, relative)) {
            error = "snapshot source path is invalid";
            return false;
        }
        const std::filesystem::path file =
            std::filesystem::weakly_canonical(fileRoot / relative, ec);
        if (ec || !IsWithin(file, fileRoot) || !std::filesystem::is_regular_file(file, ec) || ec) {
            error = "snapshot source is unavailable";
            return false;
        }
        const std::uintmax_t size = std::filesystem::file_size(file, ec);
        if (ec || size != source.size || platform::Sha256FileHex(file) != source.sha256) {
            error = "snapshot source integrity check failed";
            return false;
        }
    }
    return true;
}

std::filesystem::path SnapshotService::SnapshotDirectory(const std::string& snapshotId) const {
    return storageRoot_ / snapshotId;
}

Json SnapshotService::ToJson(const SnapshotRecord& snapshot) {
    Json sources = Json::array();
    for (const auto& source : snapshot.sources) {
        sources.push_back(Json{{"source_id", source.sourceId},
                               {"path", source.relativePath},
                               {"sha256", source.sha256},
                               {"size", std::to_string(source.size)},
                               {"origin", source.origin}});
    }
    return Json{{"schema_version", "edu.snapshot.v1"},
                {"snapshot_id", snapshot.id},
                {"project_id", snapshot.projectId},
                {"revision", snapshot.revision},
                {"created_at", snapshot.createdAt},
                {"created_at_epoch", std::to_string(snapshot.createdAtEpoch)},
                {"input_fingerprint", snapshot.inputFingerprint},
                {"top", snapshot.top},
                {"sources", sources},
                {"target", snapshot.target},
                {"tool_config", snapshot.toolConfig}};
}

bool SnapshotService::List(const std::string& projectId, std::vector<SnapshotRecord>& out,
                           std::string& error) const {
    out.clear();
    error.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(storageRoot_, ec)) {
        if (ec) {
            error = "snapshot storage is unavailable";
            return false;
        }
        return true;  // 空存储视为空列表。
    }
    for (std::filesystem::directory_iterator it(storageRoot_, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_directory(ec) || ec) continue;
        const std::string id = it->path().filename().string();
        if (id.rfind("snap-", 0) != 0) continue;
        SnapshotRecord record;
        std::string lookupError;
        if (!Lookup(id, record, lookupError)) continue;  // 损坏目录跳过（Prune 已有失败关闭，这里只读列举）。
        if (!projectId.empty() && record.projectId != projectId) continue;
        out.push_back(std::move(record));
    }
    if (ec) {
        error = "unable to enumerate snapshot storage";
        return false;
    }
    std::sort(out.begin(), out.end(), [](const SnapshotRecord& left, const SnapshotRecord& right) {
        if (left.createdAtEpoch != right.createdAtEpoch) {
            return left.createdAtEpoch > right.createdAtEpoch;
        }
        return left.id > right.id;
    });
    return true;
}

bool SnapshotService::Prune(const std::string& projectId,
                            const std::set<std::string>& protectedIds,
                            std::size_t keepRecent, std::uint64_t maxAgeSeconds,
                            std::size_t& removed, std::string& error, std::uint64_t nowEpoch) {
    // 复用全局 Prune；按 projectId 过滤记录并保护其他工程的快照。
    error.clear();
    std::error_code ec;
    if (projectId.empty()) {
        return Prune(protectedIds, keepRecent, maxAgeSeconds, removed, error, nowEpoch);
    }
    if (!std::filesystem::is_directory(storageRoot_, ec) && ec) {
        error = "snapshot storage is unavailable";
        return false;
    }
    // 其他工程的全部快照一律保护。
    std::set<std::string> scopedProtected = protectedIds;
    if (!std::filesystem::exists(storageRoot_, ec)) return !ec;
    for (std::filesystem::directory_iterator it(storageRoot_, ec), end; it != end && !ec;
         it.increment(ec)) {
        if (!it->is_directory(ec) || ec) continue;
        const std::string id = it->path().filename().string();
        if (id.rfind("snap-", 0) != 0) continue;
        SnapshotRecord record;
        std::string lookupError;
        if (!Lookup(id, record, lookupError)) continue;  // 损坏记录不动它，也不保护。
        if (record.projectId != projectId) scopedProtected.insert(record.id);
    }
    if (ec) {
        error = "unable to enumerate snapshot storage";
        return false;
    }
    return Prune(scopedProtected, keepRecent, maxAgeSeconds, removed, error, nowEpoch);
}

bool SnapshotService::Prune(const std::set<std::string>& protectedIds,
                            std::size_t keepRecent, std::uint64_t maxAgeSeconds,
                            std::size_t& removed, std::string& error,
                            std::uint64_t nowEpoch) {
    removed = 0;
    error.clear();
    if (nowEpoch == 0) nowEpoch = NowEpoch();
    std::error_code ec;
    if (!std::filesystem::exists(storageRoot_, ec)) return !ec;
    if (ec || !std::filesystem::is_directory(storageRoot_, ec) || ec) {
        error = "snapshot storage is unavailable";
        return false;
    }

    std::vector<SnapshotRecord> records;
    for (std::filesystem::directory_iterator it(storageRoot_, ec), end; it != end && !ec;
         it.increment(ec)) {
        if (!it->is_directory(ec) || ec) continue;
        const std::string id = it->path().filename().string();
        if (id.rfind("snap-", 0) != 0) continue;
        SnapshotRecord record;
        std::string lookupError;
        if (!Lookup(id, record, lookupError)) {
            error = "unable to validate snapshot before pruning";
            return false;
        }
        records.push_back(std::move(record));
    }
    if (ec) {
        error = "unable to enumerate snapshot storage";
        return false;
    }
    std::sort(records.begin(), records.end(), [](const SnapshotRecord& left,
                                                  const SnapshotRecord& right) {
        if (left.createdAtEpoch != right.createdAtEpoch) {
            return left.createdAtEpoch > right.createdAtEpoch;
        }
        return left.id > right.id;
    });

    for (std::size_t index = 0; index < records.size(); ++index) {
        const SnapshotRecord& record = records[index];
        if (protectedIds.find(record.id) != protectedIds.end()) continue;
        if (index < keepRecent) continue;
        // Legacy manifests without a trustworthy creation epoch are never deleted automatically.
        if (record.createdAtEpoch == 0) continue;
        const std::uint64_t age = nowEpoch > record.createdAtEpoch
                                      ? nowEpoch - record.createdAtEpoch
                                      : 0;
        if (age <= maxAgeSeconds) continue;
        std::filesystem::remove_all(SnapshotDirectory(record.id), ec);
        if (ec) {
            error = "unable to remove expired snapshot";
            return false;
        }
        ++removed;
    }
    return true;
}

const char* SnapshotErrorCode(SnapshotError error) {
    switch (error) {
        case SnapshotError::None: return "OK";
        case SnapshotError::InvalidArgument: return "INVALID_ARGUMENT";
        case SnapshotError::StaleRevision: return "STALE_REVISION";
        case SnapshotError::DirtyProject: return "DIRTY_PROJECT";
        case SnapshotError::SyncInProgress: return "SYNC_IN_PROGRESS";
        case SnapshotError::SourceUnavailable: return "SOURCE_UNAVAILABLE";
        case SnapshotError::PathOutsideProject: return "PATH_OUTSIDE_PROJECT";
        case SnapshotError::SourceChanged: return "SOURCE_CHANGED";
        case SnapshotError::StorageError: return "STORAGE_ERROR";
        case SnapshotError::NotFound: return "NOT_FOUND";
    }
    return "UNKNOWN";
}

} // namespace agent
} // namespace eda
