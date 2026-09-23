#include <eda/api/command.hpp>

#include "eda-core/CommandBus.h"

#include <iostream>
#include <memory>
#include <string>

namespace {

int g_failures = 0;

void Check(bool ok, const char* msg) {
    if (ok) {
        std::cout << "  ok: " << msg << "\n";
    } else {
        ++g_failures;
        std::cout << "  FAIL: " << msg << "\n";
    }
}

class CounterCommand final : public eda::ICommand {
public:
    CounterCommand(int* counter, int delta, std::string name)
        : counter_(counter), delta_(delta), name_(std::move(name)) {}

    bool Execute() override {
        *counter_ += delta_;
        return true;
    }
    void Undo() override { *counter_ -= delta_; }
    std::string Name() const override { return name_; }

private:
    int* counter_;
    int delta_;
    std::string name_;
};

class FailingCommand final : public eda::ICommand {
public:
    bool Execute() override { return false; }
    void Undo() override {}
    std::string Name() const override { return "failing"; }
};

} // namespace

int main() {
    eda::CommandBus bus;
    int counter = 0;

    Check(!bus.CanUndo() && !bus.CanRedo(), "bus starts empty");
    Check(bus.Execute(std::make_unique<CounterCommand>(&counter, 5, "add5")), "execute add5");
    Check(counter == 5, "counter after execute");
    Check(bus.UndoDepth() == 1 && bus.CanUndo(), "undo depth 1");

    Check(bus.Execute(std::make_unique<CounterCommand>(&counter, 3, "add3")), "execute add3");
    Check(counter == 8, "counter after second execute");

    Check(bus.Undo(), "undo");
    Check(counter == 5, "counter after undo");
    Check(bus.RedoDepth() == 1 && bus.CanRedo(), "redo available");

    Check(bus.Redo(), "redo");
    Check(counter == 8, "counter after redo");
    Check(bus.RedoDepth() == 0, "redo consumed");

    // 新命令清空 redo 栈
    Check(bus.Undo(), "undo again");
    Check(bus.CanRedo(), "redo available before new execute");
    Check(bus.Execute(std::make_unique<CounterCommand>(&counter, 1, "add1")), "execute add1");
    Check(!bus.CanRedo(), "new execute clears redo stack");
    Check(counter == 6, "counter after undo+new execute");

    // 失败命令不入栈
    const std::size_t depth = bus.UndoDepth();
    Check(!bus.Execute(std::make_unique<FailingCommand>()), "failing command returns false");
    Check(bus.UndoDepth() == depth, "failing command not pushed");

    Check(!bus.Execute(nullptr), "null command rejected");

    bus.Clear();
    Check(bus.UndoDepth() == 0 && bus.RedoDepth() == 0, "clear empties stacks");
    Check(!bus.Undo() && !bus.Redo(), "undo/redo on empty returns false");

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
