#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace eda {
namespace cst {

// 自 `main/fpga/CstValidator` 移入并去 wx 化。
struct CstValidationResult {
    bool valid = false;
    bool fileExists = false;
    bool fileNonEmpty = false;
    bool syntaxOk = false;
    std::string cstPath;
    std::string errorSummary;

    struct LineError {
        int line = 0;
        std::string content;
        std::string reason;
    };
    std::vector<LineError> lineErrors;

    std::vector<std::string> boundPorts;

    int totalLines = 0;
    int ioLocCount = 0;
    int ioPortCount = 0;
    int commentLines = 0;
    int invalidLines = 0;
};

class CstFileValidator {
public:
    // 4 级兜底自动解析 CST 路径。
    static std::filesystem::path AutoResolveCst(const std::filesystem::path& projectRoot,
                                                const std::string& configuredCstPath);
    // 4 步校验：存在 → 非空 → 逐行语法 → 端口覆盖。
    static CstValidationResult Validate(const std::string& cstPath,
                                        const std::vector<std::string>& allPorts = {});
    static bool ExistsAndReadable(const std::string& cstPath);

private:
    static bool ValidateSingleLine(const std::string& line, int lineNum,
                                   CstValidationResult& result);
};

} // namespace cst
} // namespace eda
