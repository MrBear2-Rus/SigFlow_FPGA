#pragma once

#include <vector>
#include <memory>
#include <string>
#include "Statement.h"

// 语句序列抽象基类，管理一组有序的 Statement
class StatementSequence {
protected:
    std::vector<std::unique_ptr<Statement>> statements_;

public:
    virtual ~StatementSequence() = default;

    // 在末尾添加一条语句
    virtual void addStatement(std::unique_ptr<Statement> stmt) = 0;

    // 在指定位置插入一条语句
    virtual void insertStatement(size_t index, std::unique_ptr<Statement> stmt) = 0;

    // 移除指定位置的语句
    virtual void removeStatement(size_t index) = 0;

    // 获取指定位置的语句（只读）
    virtual const Statement* getStatement(size_t index) const = 0;

    // 获取语句数量
    virtual size_t getStatementCount() const = 0;

    // 遍历所有语句，更新信号名
    virtual void updateSignalName(const std::string& oldName, const std::string& newName) {
        for (auto& stmt : statements_) {
            stmt->updateSignalName(oldName, newName);
        }
    }
};
