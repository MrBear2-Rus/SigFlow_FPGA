#include "CommandBus.h"

#include <utility>

namespace eda {

bool CommandBus::Execute(std::unique_ptr<ICommand> command) {
    if (!command) return false;
    if (!command->Execute()) return false;
    undoStack_.push_back(std::move(command));
    redoStack_.clear();
    return true;
}

bool CommandBus::Undo() {
    if (undoStack_.empty()) return false;
    std::unique_ptr<ICommand> command = std::move(undoStack_.back());
    undoStack_.pop_back();
    command->Undo();
    redoStack_.push_back(std::move(command));
    return true;
}

bool CommandBus::Redo() {
    if (redoStack_.empty()) return false;
    std::unique_ptr<ICommand> command = std::move(redoStack_.back());
    redoStack_.pop_back();
    if (!command->Execute()) {
        // 重做失败：丢弃该命令，避免状态不一致。
        return false;
    }
    undoStack_.push_back(std::move(command));
    return true;
}

void CommandBus::Clear() {
    undoStack_.clear();
    redoStack_.clear();
}

} // namespace eda
