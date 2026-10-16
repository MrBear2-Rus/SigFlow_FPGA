#pragma once

#include "../trace/TraceSource.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sigflow::wave {

// A protocol event is deliberately independent of the GUI.  The same event
// list can be shown in the waveform panel, exported as evidence, or attached
// to a first-difference report.
struct ProtocolTransaction {
    std::string protocol;
    sigflow::trace::TimeValue startTime = 0;
    sigflow::trace::TimeValue endTime = 0;
    bool valid = false;
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint64_t> words;
    std::uint64_t waitCycles = 0;
    std::string detail;
};

struct ProtocolDecodeOptions {
    sigflow::trace::TimeValue startTime = 0;
    sigflow::trace::TimeValue endTime = 0; // 0 = source end
    // Optional time span for bounded TraceSource queries.  Zero keeps the
    // single-query path; non-zero exercises the decoder's cross-chunk state.
    sigflow::trace::TimeValue queryChunkSize = 0;
    bool requirePhysicalTime = true;
    bool includeInvalid = true;
    // Optional cooperative cancellation hook for background/UI callers. The
    // decoder checks it between bounded queries and transitions.
    std::function<bool()> shouldCancel;
};

struct UartDecodeConfig {
    int signalId = -1;
    std::uint64_t baudRate = 0;
    unsigned dataBits = 8;
    enum class Parity { None, Even, Odd } parity = Parity::None;
    unsigned stopBits = 1;
    bool invert = false;
    ProtocolDecodeOptions options;
};

struct SpiDecodeConfig {
    int clockSignalId = -1;
    int chipSelectSignalId = -1; // negative means continuously selected
    int mosiSignalId = -1;
    int misoSignalId = -1;
    bool chipSelectActiveLow = true;
    bool clockPolarity = false;  // CPOL
    bool clockPhase = false;     // CPHA
    bool lsbFirst = false;
    unsigned bitsPerWord = 8;
    ProtocolDecodeOptions options;
};

struct ValidReadyDecodeConfig {
    int clockSignalId = -1;
    int validSignalId = -1;
    int readySignalId = -1;
    int dataSignalId = -1;
    bool risingEdge = true;
    unsigned dataWidth = 0; // 0 = infer from the signal declaration
    unsigned dataBitOffset = 0;
    ProtocolDecodeOptions options;
};

struct I2cDecodeConfig {
    int sclSignalId = -1;
    int sdaSignalId = -1;
    bool addressTenBit = false; // 首版保留 7-bit 地址；10-bit 作为显式边界拒绝
    ProtocolDecodeOptions options;
};

struct AxiLiteDecodeConfig {
    int clockSignalId = -1;
    int awValidSignalId = -1;
    int awReadySignalId = -1;
    int awAddrSignalId = -1;
    int wValidSignalId = -1;
    int wReadySignalId = -1;
    int wDataSignalId = -1;
    int bValidSignalId = -1;
    int bReadySignalId = -1;
    int bRespSignalId = -1;
    int arValidSignalId = -1;
    int arReadySignalId = -1;
    int arAddrSignalId = -1;
    int rValidSignalId = -1;
    int rReadySignalId = -1;
    int rDataSignalId = -1;
    int rRespSignalId = -1;
    bool risingEdge = true;
    ProtocolDecodeOptions options;
};

bool DecodeUart(sigflow::trace::TraceSource& source, const UartDecodeConfig& config,
                std::vector<ProtocolTransaction>& out, std::string& error);

bool DecodeSpi(sigflow::trace::TraceSource& source, const SpiDecodeConfig& config,
               std::vector<ProtocolTransaction>& out, std::string& error);

bool DecodeValidReady(sigflow::trace::TraceSource& source,
                      const ValidReadyDecodeConfig& config,
                      std::vector<ProtocolTransaction>& out, std::string& error);

bool DecodeI2c(sigflow::trace::TraceSource& source, const I2cDecodeConfig& config,
               std::vector<ProtocolTransaction>& out, std::string& error);

bool DecodeAxiLite(sigflow::trace::TraceSource& source,
                   const AxiLiteDecodeConfig& config,
                   std::vector<ProtocolTransaction>& out, std::string& error);

} // namespace sigflow::wave
