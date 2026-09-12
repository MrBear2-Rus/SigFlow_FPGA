#pragma once

#include <string>
#include <vector>

// 语句抽象基类，代表一个可执行的语句（如阻塞/非阻塞赋值）
class Statement {
public:
    virtual ~Statement() = default;

    // 获取此语句读取的信号名列表
    virtual std::vector<std::string> getReadSignalNames() const = 0;

    // 获取此语句写入的信号名列表
    virtual std::vector<std::string> getWriteSignalNames() const = 0;

    // 当某个信号被重命名时，更新语句内部对该信号的引用
    // oldName: 原信号名
    // newName: 新信号名
    virtual void updateSignalName(const std::string& oldName, const std::string& newName) = 0;

    // 赋值类型枚举
    enum class AssignmentType {
        NONE,           // 非赋值语句（如 if、case 等，暂不支持）
        BLOCKING,       // 阻塞赋值 =
        NONBLOCKING,    // 非阻塞赋值 <=
        CONTINUOUS      // 连续赋值 assign（用于 continuous_assign 节点）
    };

    // 获取此语句的赋值类型，默认返回 NONE
    virtual AssignmentType getAssignmentType() const { return AssignmentType::NONE; }

    virtual std::unique_ptr<Statement> clone() const = 0;
};
