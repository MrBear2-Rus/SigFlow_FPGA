#include "debug/DebugProtocol.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::vector<std::uint8_t> CorruptCrc(std::uint8_t type, std::uint8_t sequence,
                                     const std::vector<std::uint8_t>& data) {
    auto payload = eda::debug::proto::MakePayload(type, sequence, data);
    payload.back() ^= 1;
    auto body = eda::debug::proto::CobsEncode(payload);
    std::vector<std::uint8_t> frame{0};
    frame.insert(frame.end(), body.begin(), body.end());
    frame.push_back(0);
    return frame;
}

class FakeTransport final : public eda::debug::ITransport {
  public:
    bool Open() override {
        open_ = true;
        return true;
    }
    void Close() override { open_ = false; }
    bool IsOpen() const override { return open_; }

    std::size_t Read(std::uint8_t* data, std::size_t size, int) override {
        const auto count = std::min(size, incoming_.size());
        for (std::size_t i = 0; i < count; ++i) {
            data[i] = incoming_.front();
            incoming_.pop_front();
        }
        return count;
    }

    bool Write(const std::uint8_t* data, std::size_t size, int) override {
        const std::vector<std::uint8_t> request(data, data + size);
        std::uint8_t requestType = 0;
        std::uint8_t sequence = 0;
        std::vector<std::uint8_t> requestData;
        if (!eda::debug::proto::FrameDecode(request, requestType, sequence, requestData))
            return false;
        ++writes_;

        std::uint8_t responseType = 0;
        std::vector<std::uint8_t> responseData;
        if (requestType == eda::debug::proto::kT_Ping) {
            responseType = eda::debug::proto::kT_Pong;
            responseData = {0x07};
        } else if (requestType == eda::debug::proto::kT_GetInfo) {
            responseType = eda::debug::proto::kT_Info;
            responseData = {1,    0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
                            0x02, 0x01, 0x00, 0x02, 0x20, 0x70};
        } else {
            return false;
        }

        std::vector<std::uint8_t> response;
        if (writes_ == 1) {
            response = CorruptCrc(responseType, sequence, responseData);
        } else {
            const auto responseSequence =
                writes_ == 2 ? static_cast<std::uint8_t>(sequence + 1) : sequence;
            response = eda::debug::proto::FrameEncode(responseType, responseSequence, responseData);
        }
        incoming_.insert(incoming_.end(), response.begin(), response.end());
        return true;
    }

    std::size_t Writes() const { return writes_; }

  private:
    bool open_ = true;
    std::size_t writes_ = 0;
    std::deque<std::uint8_t> incoming_;
};

}  // namespace

int main() {
    try {
        const std::vector<std::uint8_t> payload = {0, 1, 0x11, 0xff, 0};
        const auto encoded =
            eda::debug::proto::FrameEncode(eda::debug::proto::kT_Ping, 42, payload);
        std::uint8_t type = 0;
        std::uint8_t sequence = 0;
        std::vector<std::uint8_t> decoded;
        Check(eda::debug::proto::FrameDecode(encoded, type, sequence, decoded), "frame roundtrip");
        Check(type == eda::debug::proto::kT_Ping && sequence == 42 && decoded == payload,
              "frame preserves binary payload");

        FakeTransport transport;
        eda::debug::DebugProtocol protocol(transport);
        std::uint8_t version = 0;
        std::string error;
        Check(protocol.Ping(version, error, 100, 3), "ping retries malformed responses");
        Check(version == 7 && transport.Writes() == 3, "ping response after retry");
        const auto stats = protocol.Stats();
        Check(stats.commandCalls == 1 && stats.attempts == 3 && stats.retries == 2,
              "retry statistics");
        Check(stats.crcErrors == 1 && stats.sequenceMismatches == 1 && stats.malformedFrames == 0,
              "response diagnostics");

        eda::debug::DebugInfo info;
        Check(protocol.GetInfo(info, error, 100, 1), "get info response");
        Check(info.ipVersion == 1 && info.fingerprint64 == 0x0102030405060708ULL &&
                  info.depth == 512 && info.width == 32 && info.flags == 0x70,
              "get info fields decoded");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
