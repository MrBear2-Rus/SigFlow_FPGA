#pragma once

#include "ITransport.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace sigflow {
namespace debug {

// 传输层指标与协议层重试/CRC 统计分开保存。这里的计数只描述 I/O，
// 不把协议帧、样本数或重试次数混进字节数。
struct TransportStats {
    std::uint64_t readCalls = 0;
    std::uint64_t readBytes = 0;
    std::uint64_t readElapsedUs = 0;
    std::uint64_t readTimeouts = 0;
    std::uint64_t readDisconnects = 0;
    std::uint64_t writeCalls = 0;
    std::uint64_t writeBytes = 0;
    std::uint64_t writeElapsedUs = 0;
    std::uint64_t writeTimeouts = 0;
    std::uint64_t writeDisconnects = 0;
    std::uint64_t discardCalls = 0;
};

// ITransport 装饰器：不改变后端的字节语义，只在宿主侧记录 I/O 统计。
class InstrumentedTransport final : public ITransport {
public:
    explicit InstrumentedTransport(std::unique_ptr<ITransport> inner)
        : inner_(std::move(inner)) {}

    bool Open() override { return inner_ && inner_->Open(); }
    void Close() override { if (inner_) inner_->Close(); }
    bool IsOpen() const override { return inner_ && inner_->IsOpen(); }

    std::size_t Read(std::uint8_t* data, std::size_t size, int timeoutMs) override
    {
        const auto started = std::chrono::steady_clock::now();
        const std::size_t got = inner_ ? inner_->Read(data, size, timeoutMs) : 0;
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
        readCalls_.fetch_add(1, std::memory_order_relaxed);
        readBytes_.fetch_add(got, std::memory_order_relaxed);
        readElapsedUs_.fetch_add(static_cast<std::uint64_t>(elapsed), std::memory_order_relaxed);
        if (got == 0) {
            if (!inner_ || !inner_->IsOpen())
                readDisconnects_.fetch_add(1, std::memory_order_relaxed);
            else
                readTimeouts_.fetch_add(1, std::memory_order_relaxed);
        }
        return got;
    }

    bool Write(const std::uint8_t* data, std::size_t size, int timeoutMs) override
    {
        const auto started = std::chrono::steady_clock::now();
        const bool ok = inner_ && inner_->Write(data, size, timeoutMs);
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
        writeCalls_.fetch_add(1, std::memory_order_relaxed);
        if (ok) {
            writeBytes_.fetch_add(size, std::memory_order_relaxed);
        } else if (!inner_ || !inner_->IsOpen()) {
            writeDisconnects_.fetch_add(1, std::memory_order_relaxed);
        } else {
            writeTimeouts_.fetch_add(1, std::memory_order_relaxed);
        }
        writeElapsedUs_.fetch_add(static_cast<std::uint64_t>(elapsed), std::memory_order_relaxed);
        return ok;
    }

    void DiscardInput() override
    {
        discardCalls_.fetch_add(1, std::memory_order_relaxed);
        if (inner_) inner_->DiscardInput();
    }

    TransportStats Stats() const
    {
        TransportStats out;
        out.readCalls = readCalls_.load(std::memory_order_relaxed);
        out.readBytes = readBytes_.load(std::memory_order_relaxed);
        out.readElapsedUs = readElapsedUs_.load(std::memory_order_relaxed);
        out.readTimeouts = readTimeouts_.load(std::memory_order_relaxed);
        out.readDisconnects = readDisconnects_.load(std::memory_order_relaxed);
        out.writeCalls = writeCalls_.load(std::memory_order_relaxed);
        out.writeBytes = writeBytes_.load(std::memory_order_relaxed);
        out.writeElapsedUs = writeElapsedUs_.load(std::memory_order_relaxed);
        out.writeTimeouts = writeTimeouts_.load(std::memory_order_relaxed);
        out.writeDisconnects = writeDisconnects_.load(std::memory_order_relaxed);
        out.discardCalls = discardCalls_.load(std::memory_order_relaxed);
        return out;
    }

private:
    std::unique_ptr<ITransport> inner_;
    std::atomic<std::uint64_t> readCalls_{0};
    std::atomic<std::uint64_t> readBytes_{0};
    std::atomic<std::uint64_t> readElapsedUs_{0};
    std::atomic<std::uint64_t> readTimeouts_{0};
    std::atomic<std::uint64_t> readDisconnects_{0};
    std::atomic<std::uint64_t> writeCalls_{0};
    std::atomic<std::uint64_t> writeBytes_{0};
    std::atomic<std::uint64_t> writeElapsedUs_{0};
    std::atomic<std::uint64_t> writeTimeouts_{0};
    std::atomic<std::uint64_t> writeDisconnects_{0};
    std::atomic<std::uint64_t> discardCalls_{0};
};

} // namespace debug
} // namespace sigflow
