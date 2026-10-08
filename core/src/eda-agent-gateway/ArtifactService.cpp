#include "ArtifactService.h"

#include "eda-platform/Sha256.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>

namespace eda {
namespace agent {

namespace {

std::string LowerExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

} // namespace

void ArtifactService::Register(const ArtifactRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    records_[record.artifactId] = record;
}

void ArtifactService::RemoveProject(const std::string& projectId) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = records_.begin(); it != records_.end();) {
        if (it->second.projectId == projectId) {
            it = records_.erase(it);
        } else {
            ++it;
        }
    }
}

bool ArtifactService::Lookup(const std::string& artifactId, ArtifactRecord& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = records_.find(artifactId);
    if (it == records_.end()) return false;
    out = it->second;
    return true;
}

std::string ArtifactService::GuessMediaType(const ArtifactRecord& record) {
    const std::string ext = LowerExtension(record.path);
    if (ext == ".json") return "application/json";
    if (ext == ".v" || ext == ".sv" || ext == ".vh") return "text/x-verilog";
    if (ext == ".log" || ext == ".txt") return "text/plain";
    if (ext == ".xml") return "application/xml";
    if (ext == ".csv") return "text/csv";
    if (ext == ".vcd") return "application/vcd";
    if (ext == ".fst") return "application/fst";
    if (ext == ".yaml" || ext == ".yml") return "application/yaml";
    return "application/octet-stream";
}

bool ArtifactService::IsInlineReadable(const std::string& mediaType, const std::string& schema) {
    // 波形/大二进制不内联（应走 /waves）；其余文本/结构化小文件可读。
    if (mediaType == "application/vcd" || mediaType == "application/fst") return false;
    if (schema.find("wave") != std::string::npos) return false;
    if (mediaType.rfind("text/", 0) == 0) return true;
    if (mediaType == "application/json" || mediaType == "application/xml" ||
        mediaType == "application/yaml" || mediaType == "application/vcd") {
        return true;
    }
    return false;
}

ArtifactService::Status ArtifactService::Describe(const std::string& artifactId, Metadata& out) {
    ArtifactRecord record;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = records_.find(artifactId);
        if (it == records_.end()) return Status::kNotFound;
        record = it->second;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(record.path, ec)) return Status::kExpired;
    if (!record.sha256.empty()) {
        const std::string current = platform::Sha256FileHex(record.path);
        if (current.empty() || current != record.sha256) return Status::kExpired;
    }
    out.artifactId = record.artifactId;
    out.projectId = record.projectId;
    out.revision = record.revision;
    out.jobId = record.jobId;
    out.schema = record.schema;
    out.sha256 = record.sha256;
    out.role = record.role;
    out.size = static_cast<std::uint64_t>(std::filesystem::file_size(record.path, ec));
    out.mediaType = GuessMediaType(record);
    out.inlineReadable = IsInlineReadable(out.mediaType, record.schema);
    return Status::kOk;
}

ArtifactService::Status ArtifactService::ReadContent(const std::string& artifactId,
                                                     std::uint64_t offset, std::uint64_t length,
                                                     std::size_t maxBytes, std::string& out,
                                                     Metadata& meta, bool& truncated) {
    const Status status = Describe(artifactId, meta);
    if (status != Status::kOk) return status;
    if (!meta.inlineReadable) return Status::kUnsupported;

    std::size_t limit = maxBytes == 0 ? kDefaultContentLimit : maxBytes;
    limit = std::min(limit, kMaxContentLimit);
    if (offset > meta.size) return Status::kBadRange;
    if (length == 0 || length > limit) length = limit;
    const std::uint64_t available = meta.size - offset;
    if (length > available) length = available;
    truncated = (offset + length) < meta.size;

    ArtifactRecord record;
    Lookup(artifactId, record);
    std::ifstream input(record.path, std::ios::binary);
    if (!input) return Status::kExpired;
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input) return Status::kBadRange;
    out.resize(static_cast<std::size_t>(length));
    input.read(&out[0], static_cast<std::streamsize>(length));
    out.resize(static_cast<std::size_t>(input.gcount()));
    return Status::kOk;
}

} // namespace agent
} // namespace eda
