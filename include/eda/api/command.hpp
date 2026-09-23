#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace eda {

// 可撤销命令。
class ICommand {
public:
    virtual ~ICommand() = default;
    virtual bool Execute() = 0;
    virtual void Undo() = 0;
    virtual std::string Name() const = 0;
};

// P2-3：命令总线（execute/undo/redo）。编辑写路径（AddChild/RemoveChild/AddWire/…）包成命令后经此提交。
class ICommandBus {
public:
    virtual ~ICommandBus() = default;

    // 执行成功才入撤销栈（返回 false 不入栈）。
    virtual bool Execute(std::unique_ptr<ICommand> command) = 0;
    virtual bool Undo() = 0;
    virtual bool Redo() = 0;
    virtual bool CanUndo() const = 0;
    virtual bool CanRedo() const = 0;
    virtual std::size_t UndoDepth() const = 0;
    virtual std::size_t RedoDepth() const = 0;
    virtual void Clear() = 0;
};

} // namespace eda
