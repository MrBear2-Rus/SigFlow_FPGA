#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace sigflow::debug {

// Wire opcodes used by the first TraceBridge JTAG capture core.  The wire
// protocol is deliberately independent from UART DebugProtocol: a JTAG TAP
// exchange carries one 64-bit command and one 64-bit response.
enum class JtagOpcode : std::uint8_t {
    Identity = 0,
    Arm = 1,
    Status = 2,
    ReadCapture = 3,
    TriggerMask = 4,
    TriggerValue = 5,
    Decimation = 6,
    GetInfo = 7,
};

enum class JtagStatus : std::uint8_t {
    Ok = 0,
    InvalidCommand = 1,
    Busy = 2,
    OutOfRange = 3,
};

struct JtagResponse {
    std::uint8_t sequence = 0;
    JtagOpcode opcode = JtagOpcode::Identity;
    JtagStatus status = JtagStatus::InvalidCommand;
    std::uint32_t data = 0;
};

bool EncodeJtagCommand(std::uint8_t sequence, JtagOpcode opcode,
                       std::uint32_t data, std::uint64_t& word,
                       std::string& error);

bool DecodeJtagResponse(std::uint64_t word, std::uint8_t expectedSequence,
                        JtagOpcode expectedOpcode, JtagResponse& response,
                        std::string& error);

// The callback is the only hardware-specific boundary.  It may perform a
// FT2232/MPSSE TAP transaction, a scripted board exchange, or a deterministic
// simulator exchange; this class owns no USB handle and never reconfigures a
// device.
using JtagExchangeCallback =
    std::function<bool(std::uint64_t command, std::uint64_t& response,
                       std::string& error)>;

class JtagDebugProtocol {
public:
    explicit JtagDebugProtocol(JtagExchangeCallback exchange);

    bool Transact(JtagOpcode opcode, std::uint32_t data,
                  JtagResponse& response, std::string& error);

    bool Identity(std::uint32_t& value, std::string& error);
    bool ConfigureTriggerMask(std::uint32_t mask, std::string& error);
    bool ConfigureTriggerValue(std::uint32_t value, std::string& error);
    bool ConfigureDecimation(std::uint8_t decimation, std::string& error);
    bool Arm(std::string& error);
    bool ReadStatus(std::uint32_t& value, std::string& error);
    bool ReadCapture(std::uint32_t address, std::uint32_t& value,
                     std::string& error);
    bool GetInfo(std::uint16_t& width, std::uint16_t& depth,
                 std::string& error);

    std::uint8_t nextSequence() const { return nextSequence_; }

private:
    bool ExpectOk(JtagOpcode opcode, std::uint32_t data,
                  JtagResponse& response, std::string& error);

    JtagExchangeCallback exchange_;
    std::uint8_t nextSequence_ = 0;
};

} // namespace sigflow::debug
