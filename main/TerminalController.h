#pragma once

#include <string>



/**
 * @brief 终端控制器
 * 负责接收终端输入的一行命令，并调用后台逻辑处理
 */
class TerminalController
{
public:
    // 构造函数：持有全局上下文
    explicit TerminalController();

    /**
     * @brief 提交一行命令
     * @param line   用户输入的原始命令
     * @param output 正常输出（返回给终端）
     * @param error  错误输出（返回给终端）
     */
    void submitLine(const std::string& line,
        std::string& output,
        std::string& error);


};
