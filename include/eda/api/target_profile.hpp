#pragma once

#include <string>

namespace eda {

// eda.target-profile.v1 的 wx-free 表示（P1-5）。
struct TargetProfile {
    std::string id;
    std::string version;
    std::string displayName;

    std::string yosysFamily;      // yosys.family
    std::string device;           // nextpnr.device
    std::string family;           // nextpnr.family
    std::string programmerBoard;  // openfpgaloader.board

    int uartTxPin = -1;           // tracebridge.uart_tx_pin
    int uartRxPin = -1;           // tracebridge.uart_rx_pin
    std::string debugReset;       // tracebridge.debug_reset
    int defaultBaud = 0;          // tracebridge.default_baud

    std::string sourcePath;       // 来源文件（可空）
};

} // namespace eda
