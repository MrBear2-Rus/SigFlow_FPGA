#pragma once

#include <eda/api/command.hpp>

#include <memory>
#include <vector>

namespace eda {

// P2-3：进程内命令总线（撤销/重做双栈）。
class CommandBus final : public ICommandBus {
public:
    bool Execute(std::unique_ptr<ICommand> command) override;
    bool Undo() override;
    bool Redo() override;
    bool CanUndo() const override { return !undoStack_.empty(); }
    bool CanRedo() const override { return !redoStack_.empty(); }
    std::size_t UndoDepth() const override { return undoStack_.size(); }
    std::size_t RedoDepth() const override { return redoStack_.size(); }
    void Clear() override;

private:
    std::vector<std::unique_ptr<ICommand>> undoStack_;
    std::vector<std::unique_ptr<ICommand>> redoStack_;
};

} // namespace eda
