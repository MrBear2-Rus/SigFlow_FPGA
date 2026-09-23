#pragma once

#include <string>
#include <vector>

namespace eda {
namespace cst {

struct CstBinding {
    std::string portName;
    int packagePin = -1;
    std::string ioType = "LVCMOS33";
    std::string drive;
    bool pullUp = false;
    bool pullDown = false;
    std::string comment;
};

struct CstError {
    std::string severity = "error";  // "error" | "warning"
    std::string message;
    int lineNumber = 0;
    bool IsError() const { return severity == "error"; }
};

// P1-6（第一增量）：Gowin CST（IO_LOC / IO_PORT）wx-free 编解码。
// 完整的 FpgaConstraint / CstValidator 移入与主程序适配在后续增量。
class GowinCstCodec {
public:
    static std::string Generate(const std::vector<CstBinding>& bindings);
    static bool Parse(const std::string& text, std::vector<CstBinding>& bindings,
                      std::vector<CstError>& errors);
};

} // namespace cst
} // namespace eda
