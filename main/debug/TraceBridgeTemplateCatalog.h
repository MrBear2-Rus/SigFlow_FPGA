#pragma once

#include <string>
#include <vector>

namespace sigflow::debug {

enum class TraceBridgeTemplateKind {
    Fifo,
    Handshake,
    Reset,
    StateMachine,
    Uart,
    Spi,
};

const char* ToString(TraceBridgeTemplateKind kind);

struct TraceBridgeTemplateEntry {
    std::string id;
    TraceBridgeTemplateKind kind = TraceBridgeTemplateKind::Fifo;
    std::string title;
    std::vector<std::string> suggestedSignals;
    std::string helpTarget;
    std::string basis;
    std::string expectedResult;
    std::string limitations;
};

// TB32-08 的静态模板/帮助目录。只提供建议和证据边界，不自动修改契约或源码。
class TraceBridgeTemplateCatalog {
public:
    static const std::vector<TraceBridgeTemplateEntry>& All();
    static bool Find(const std::string& id, TraceBridgeTemplateEntry& entry,
                     std::string& error);
};

} // namespace sigflow::debug
