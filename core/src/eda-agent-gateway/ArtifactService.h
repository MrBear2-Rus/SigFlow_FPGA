#pragma once

#include <eda/api/Types.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

namespace eda {
namespace agent {

// 已登记 artifact 的绑定记录（Job 产出后由宿主登记）。
struct ArtifactRecord {
    std::string artifactId;
    std::string projectId;
    std::string revision;
    std::string jobId;
    std::filesystem::path path;
    std::string schema;
    std::string sha256;
    std::string role;
};

// SF-06：artifact 元数据与受限内容读取（不把整个波形/大文件交给模型）。
class ArtifactService {
public:
    void Register(const ArtifactRecord& record);
    void RemoveProject(const std::string& projectId);
    bool Lookup(const std::string& artifactId, ArtifactRecord& out) const;

    struct Metadata {
        std::string artifactId;
        std::string projectId;
        std::string revision;
        std::string jobId;
        std::string schema;
        std::string sha256;
        std::string role;
        std::uint64_t size = 0;
        std::string mediaType;    // 由扩展名/schema 推导
        bool inlineReadable = false;  // 是否允许 /content 读取
    };

    enum class Status { kOk, kNotFound, kExpired, kUnsupported, kBadRange };

    Status Describe(const std::string& artifactId, Metadata& out);

    // 有界字节读取；offset/length 以字节计。length 为 0 用默认上限。
    Status ReadContent(const std::string& artifactId, std::uint64_t offset, std::uint64_t length,
                       std::size_t maxBytes, std::string& out, Metadata& meta, bool& truncated);

    // 单次内容响应上限（普通响应 2 MiB 的精简上限）。
    static constexpr std::size_t kDefaultContentLimit = 256 * 1024;
    static constexpr std::size_t kMaxContentLimit = 1024 * 1024;

private:
    static std::string GuessMediaType(const ArtifactRecord& record);
    static bool IsInlineReadable(const std::string& mediaType, const std::string& schema);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ArtifactRecord> records_;
};

} // namespace agent
} // namespace eda
