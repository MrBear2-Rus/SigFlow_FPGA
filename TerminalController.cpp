#include "TerminalController.h"

#include <sstream>
#include <vector>

/**
 * 构造函数
 */
TerminalController::TerminalController()
    
{
}

/**
 * 提交一行命令
 */
void TerminalController::submitLine(const std::string& line,
    std::string& output,
    std::string& error)
{
    output.clear();
    error.clear();

    // 1. 去掉空行
    if (line.empty()) {
        return;
    }

    // 2. 暂时不做复杂解析，直接 echo（占位）
    if (line == "help") {
        output =
            "Available commands:\n"
            "  help        show this message\n"
            "  echo <msg>  echo message\n";
        return;
    }

    // 3. 一个简单示例命令
    if (line.rfind("echo ", 0) == 0) {
        output = line.substr(5);
        return;
    }

    // 4. 你之后在这里接入 SigFlow 的真实逻辑
    error = "Unknown command: " + line;
}
