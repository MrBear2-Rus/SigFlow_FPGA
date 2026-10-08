#pragma once

#include <eda/api/Types.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace eda {
namespace agent {

// NG-09：旧 `main/jobs` 历史的**只读**桥。
//
// 设计边界（不可放宽）：
//   * 只读：绝不修改、迁移或"补齐"旧 `<project>/.sigflow/jobs/<type>/<jobId>/` 目录；
//     新格式不为了迁就旧记录而改动。
//   * 旧记录没有 input_fingerprint / revision / plugin 版本，因此归一化结果一律
//     `origin="legacy"`、`completeness="legacy_unverified"`，不得冒充当前 revision 的证据。
//   * 旧 manifest 的 `parameters` 含本机绝对路径与可执行文件，桥**从不**返回该字段；
//     报告里的产物只给 opaque artifact_id + sha256，不返回本机路径。
//   * 缺报告/解析失败是显式状态，不等于"电路无问题"。
struct LegacyJobEntry {
    std::string jobId;
    std::string jobType;  // 目录名：simulation/synthesis/pack/flash/...
    std::string state;
    std::string createdAt;
    std::string updatedAt;
    bool hasManifest = false;
    bool hasReport = false;
    // 空表示可读；否则为 report_missing / manifest_unreadable / report_unreadable。
    std::string problem;
};

class LegacyJobBridge {
public:
    static constexpr std::size_t kDefaultLimit = 100;
    static constexpr std::size_t kMaxLimit = 500;

    // projectRoot 为工程根目录；桥只访问其 `.sigflow/jobs` 子树。
    explicit LegacyJobBridge(std::filesystem::path projectRoot);

    // 列出旧 Job（createdAt 降序、jobId 降序；时间缺失的排最后）。
    bool List(std::vector<LegacyJobEntry>& out, std::string& error) const;

    // 读取一条旧记录并归一化为 edu.jobreport.v1。
    // 失败时 error 为 machine-readable：legacy_job_not_found / report_missing /
    // report_unreadable。成功时 normalized 一定带 completeness="legacy_unverified"。
    bool ReadReport(const std::string& jobId, const std::string& projectId, Json& normalized,
                    std::string& error) const;

    static Json ToJson(const LegacyJobEntry& entry);

private:
    bool FindJobDirectory(const std::string& jobId, std::filesystem::path& directory,
                          std::string& jobType) const;

    std::filesystem::path projectRoot_;
};

} // namespace agent
} // namespace eda
