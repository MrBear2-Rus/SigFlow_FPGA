#include "TraceBridgeTemplateCatalog.h"

#include <utility>

namespace sigflow::debug {

const char* ToString(TraceBridgeTemplateKind kind)
{
    switch (kind) {
        case TraceBridgeTemplateKind::Fifo: return "fifo";
        case TraceBridgeTemplateKind::Handshake: return "handshake";
        case TraceBridgeTemplateKind::Reset: return "reset";
        case TraceBridgeTemplateKind::StateMachine: return "state_machine";
        case TraceBridgeTemplateKind::Uart: return "uart";
        case TraceBridgeTemplateKind::Spi: return "spi";
    }
    return "fifo";
}

const std::vector<TraceBridgeTemplateEntry>& TraceBridgeTemplateCatalog::All()
{
    static const std::vector<TraceBridgeTemplateEntry> entries = {
        {"fifo", TraceBridgeTemplateKind::Fifo, "FIFO occupancy and boundary",
         {"wr_en", "rd_en", "full", "empty", "count", "data"},
         "tb08:fifo",
         "FIFO behavior is best explained by write/read enables, full/empty and occupancy together.",
         "identify the first illegal write/read or an occupancy transition that violates the contract",
         "a trace cannot prove memory initialization, CDC safety or data integrity outside the captured signals"},
        {"handshake", TraceBridgeTemplateKind::Handshake, "Valid/ready transaction",
         {"valid", "ready", "payload", "last", "stall"},
         "tb08:valid-ready",
         "a transfer occurs only on a cycle where valid and ready are both asserted",
         "show the first accepted payload, stall window and any payload change while valid is held",
         "without the producer/consumer clock and payload qualifier, a trace cannot assign protocol ownership"},
        {"reset", TraceBridgeTemplateKind::Reset, "Reset release and first activity",
         {"clk", "rst", "rst_n", "state", "valid", "ready"},
         "tb08:reset",
         "reset polarity, synchronizer release and first post-reset transaction must be checked together",
         "show release latency, unexpected activity during reset and the first valid state transition",
         "a short window cannot establish power-on behavior, analog reset quality or every clock domain's release"},
        {"state-machine", TraceBridgeTemplateKind::StateMachine, "State transition and dwell",
         {"state", "state_next", "event", "error", "enable"},
         "tb08:state-machine",
         "state plus transition cause makes illegal transitions and unexpected dwell time explainable",
         "locate the first unexpected state, missing event or transition that never becomes reachable",
         "state encoding alone cannot prove the intended RTL transition condition or unreachable-state coverage"},
        {"uart", TraceBridgeTemplateKind::Uart, "UART frame and sampling",
         {"tx", "rx", "baud_tick", "bit_index", "busy", "valid"},
         "tb14:uart",
         "UART decoding needs a stable sample clock/baud relationship and a frame boundary",
         "decode start/data/stop bits and correlate a frame with the DUT valid event",
         "without the baud reference and line level, a decoded byte cannot prove external link integrity"},
        {"spi", TraceBridgeTemplateKind::Spi, "SPI transaction and mode",
         {"sclk", "mosi", "miso", "cs_n", "bit_index", "valid"},
         "tb14:spi",
         "SPI interpretation depends on chip-select framing, clock edge and bit order",
         "show transaction boundaries, sampled bits and the first command/response mismatch",
         "a capture without explicit CPOL/CPHA, frequency or chip-select context cannot uniquely decode every frame"},
    };
    return entries;
}

bool TraceBridgeTemplateCatalog::Find(const std::string& id,
                                      TraceBridgeTemplateEntry& entry,
                                      std::string& error)
{
    error.clear();
    for (const auto& candidate : All()) {
        if (candidate.id == id) {
            entry = candidate;
            return true;
        }
    }
    error = "unknown TraceBridge template: " + id;
    return false;
}

} // namespace sigflow::debug
