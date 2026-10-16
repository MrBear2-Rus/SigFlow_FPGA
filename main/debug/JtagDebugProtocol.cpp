#include "JtagDebugProtocol.h"

#include <iomanip>
#include <sstream>
#include <utility>

namespace sigflow::debug {
namespace {

constexpr std::uint8_t kCommandMagic = 0xA5;
constexpr std::uint8_t kResponseMagic = 0x5A;

const char* OpcodeName(JtagOpcode opcode)
{
    switch (opcode) {
        case JtagOpcode::Identity: return "identity";
        case JtagOpcode::Arm: return "arm";
        case JtagOpcode::Status: return "status";
        case JtagOpcode::ReadCapture: return "read_capture";
        case JtagOpcode::TriggerMask: return "trigger_mask";
        case JtagOpcode::TriggerValue: return "trigger_value";
        case JtagOpcode::Decimation: return "decimation";
        case JtagOpcode::GetInfo: return "get_info";
    }
    return "unknown";
}

std::string HexWord(std::uint64_t word)
{
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setw(16)
        << std::setfill('0') << word;
    return out.str();
}

} // namespace

bool EncodeJtagCommand(std::uint8_t sequence, JtagOpcode opcode,
                       std::uint32_t data, std::uint64_t& word,
                       std::string& error)
{
    error.clear();
    word = (static_cast<std::uint64_t>(kCommandMagic) << 56) |
           (static_cast<std::uint64_t>(sequence) << 48) |
           (static_cast<std::uint64_t>(static_cast<std::uint8_t>(opcode)) << 40) |
           static_cast<std::uint64_t>(data);
    return true;
}

bool DecodeJtagResponse(std::uint64_t word, std::uint8_t expectedSequence,
                        JtagOpcode expectedOpcode, JtagResponse& response,
                        std::string& error)
{
    error.clear();
    const auto magic = static_cast<std::uint8_t>((word >> 56) & 0xFF);
    const auto sequence = static_cast<std::uint8_t>((word >> 48) & 0xFF);
    const auto opcode = static_cast<JtagOpcode>((word >> 40) & 0xFF);
    const auto status = static_cast<JtagStatus>((word >> 32) & 0xFF);
    if (magic != kResponseMagic) {
        error = "JTAG response magic mismatch: " + HexWord(word);
        return false;
    }
    if (sequence != expectedSequence) {
        error = "JTAG response sequence mismatch: expected " +
                std::to_string(expectedSequence) + ", got " +
                std::to_string(sequence);
        return false;
    }
    if (opcode != expectedOpcode) {
        error = "JTAG response opcode mismatch: expected " +
                std::string(OpcodeName(expectedOpcode)) + ", got " +
                std::string(OpcodeName(opcode));
        return false;
    }
    response.sequence = sequence;
    response.opcode = opcode;
    response.status = status;
    response.data = static_cast<std::uint32_t>(word & 0xFFFFFFFFu);
    return true;
}

JtagDebugProtocol::JtagDebugProtocol(JtagExchangeCallback exchange)
    : exchange_(std::move(exchange))
{
}

bool JtagDebugProtocol::Transact(JtagOpcode opcode, std::uint32_t data,
                                 JtagResponse& response, std::string& error)
{
    error.clear();
    if (!exchange_) {
        error = "JTAG exchange callback is not configured";
        return false;
    }
    ++nextSequence_;
    std::uint64_t command = 0;
    if (!EncodeJtagCommand(nextSequence_, opcode, data, command, error)) return false;
    std::uint64_t rawResponse = 0;
    if (!exchange_(command, rawResponse, error)) {
        if (error.empty()) error = "JTAG exchange failed";
        return false;
    }
    return DecodeJtagResponse(rawResponse, nextSequence_, opcode, response, error);
}

bool JtagDebugProtocol::ExpectOk(JtagOpcode opcode, std::uint32_t data,
                                 JtagResponse& response, std::string& error)
{
    if (!Transact(opcode, data, response, error)) return false;
    if (response.status != JtagStatus::Ok) {
        error = "JTAG " + std::string(OpcodeName(opcode)) +
                " returned status " + std::to_string(static_cast<unsigned>(response.status));
        return false;
    }
    return true;
}

bool JtagDebugProtocol::Identity(std::uint32_t& value, std::string& error)
{
    JtagResponse response;
    if (!ExpectOk(JtagOpcode::Identity, 0, response, error)) return false;
    value = response.data;
    return true;
}

bool JtagDebugProtocol::ConfigureTriggerMask(std::uint32_t mask, std::string& error)
{
    JtagResponse response;
    return ExpectOk(JtagOpcode::TriggerMask, mask, response, error);
}

bool JtagDebugProtocol::ConfigureTriggerValue(std::uint32_t value, std::string& error)
{
    JtagResponse response;
    return ExpectOk(JtagOpcode::TriggerValue, value, response, error);
}

bool JtagDebugProtocol::ConfigureDecimation(std::uint8_t decimation, std::string& error)
{
    if (decimation == 0) {
        error = "JTAG decimation must be greater than zero";
        return false;
    }
    JtagResponse response;
    return ExpectOk(JtagOpcode::Decimation, decimation, response, error);
}

bool JtagDebugProtocol::Arm(std::string& error)
{
    JtagResponse response;
    return ExpectOk(JtagOpcode::Arm, 0, response, error);
}

bool JtagDebugProtocol::ReadStatus(std::uint32_t& value, std::string& error)
{
    JtagResponse response;
    if (!ExpectOk(JtagOpcode::Status, 0, response, error)) return false;
    value = response.data;
    return true;
}

bool JtagDebugProtocol::ReadCapture(std::uint32_t address, std::uint32_t& value,
                                    std::string& error)
{
    JtagResponse response;
    if (!ExpectOk(JtagOpcode::ReadCapture, address, response, error)) return false;
    value = response.data;
    return true;
}

bool JtagDebugProtocol::GetInfo(std::uint16_t& width, std::uint16_t& depth,
                                std::string& error)
{
    JtagResponse response;
    if (!ExpectOk(JtagOpcode::GetInfo, 0, response, error)) return false;
    width = static_cast<std::uint16_t>((response.data >> 16) & 0xFFFF);
    depth = static_cast<std::uint16_t>(response.data & 0xFFFF);
    if (width == 0 || depth == 0) {
        error = "JTAG GET_INFO returned invalid geometry";
        return false;
    }
    return true;
}

} // namespace sigflow::debug
