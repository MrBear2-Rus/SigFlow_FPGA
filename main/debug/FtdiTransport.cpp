#include "FtdiTransport.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <vector>

#if defined(SIGFLOW_HAVE_LIBFTDI)
#include <libftdi1/ftdi.h>
#endif

namespace sigflow {
namespace debug {

#if !defined(SIGFLOW_HAVE_LIBFTDI)

// 本构建没有 libftdi：该传输层不可用，调用方回退到串口传输层。
struct FtdiTransport::Impl {};
FtdiTransport::FtdiTransport(std::uint16_t, std::uint16_t, int, std::uint32_t)
    : impl_(new Impl) {}
FtdiTransport::~FtdiTransport() = default;
bool FtdiTransport::Open() { return false; }
void FtdiTransport::Close() {}
bool FtdiTransport::IsOpen() const { return false; }
std::size_t FtdiTransport::Read(std::uint8_t*, std::size_t, int) { return 0; }
bool FtdiTransport::Write(const std::uint8_t*, std::size_t, int) { return false; }
void FtdiTransport::DiscardInput() {}
bool FtdiTransport::IsSupported() { return false; }
bool FtdiTransport::IsDevicePresent(std::uint16_t, std::uint16_t) { return false; }

#else

namespace {
// 协议帧是 0x00 ... 0x00 定界；实测 BL702 会在流里注入 0x11 状态字节。
// 只丢弃【帧外】的 0x11，帧内字节一律保留（COBS 帧里 0x11 可能是合法数据）。
constexpr std::uint8_t kDelimiter = 0x00;
constexpr std::uint8_t kInjectedStatus = 0x11;
} // namespace

struct FtdiTransport::Impl {
    ftdi_context* ctx = nullptr;
    std::uint16_t vendorId = 0x0403;
    std::uint16_t productId = 0x6010;
    int interfaceIndex = 1;
    std::uint32_t baud = 921600;
    std::deque<std::uint8_t> pending;
    bool inFrame = false;
};

FtdiTransport::FtdiTransport(std::uint16_t vendorId, std::uint16_t productId,
                             int interfaceIndex, std::uint32_t baud)
    : impl_(new Impl)
{
    impl_->vendorId = vendorId;
    impl_->productId = productId;
    impl_->interfaceIndex = interfaceIndex;
    impl_->baud = baud;
}

FtdiTransport::~FtdiTransport() { Close(); }

bool FtdiTransport::Open()
{
    if (impl_->ctx) return true;

    ftdi_context* ctx = ftdi_new();
    if (!ctx) return false;
    ctx->usb_read_timeout = 200;
    ctx->usb_write_timeout = 200;
    ftdi_set_interface(ctx, impl_->interfaceIndex == 0 ? INTERFACE_A : INTERFACE_B);
    if (ftdi_usb_open(ctx, impl_->vendorId, impl_->productId) != 0) {
        ftdi_free(ctx);
        return false;
    }
    ftdi_set_baudrate(ctx, static_cast<int>(impl_->baud));
    ftdi_set_line_property(ctx, BITS_8, STOP_BIT_1, NONE);
    ftdi_setflowctrl(ctx, SIO_DISABLE_FLOW_CTRL);
    ftdi_usb_purge_buffers(ctx);

    impl_->ctx = ctx;
    impl_->pending.clear();
    impl_->inFrame = false;
    return true;
}

void FtdiTransport::Close()
{
    if (impl_->ctx) {
        ftdi_usb_close(impl_->ctx);
        ftdi_free(impl_->ctx);
        impl_->ctx = nullptr;
    }
    impl_->pending.clear();
    impl_->inFrame = false;
}

bool FtdiTransport::IsOpen() const { return impl_->ctx != nullptr; }

std::size_t FtdiTransport::Read(std::uint8_t* data, std::size_t size, int timeoutMs)
{
    if (!impl_->ctx || !data || size == 0) return 0;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(1, timeoutMs));
    while (impl_->pending.empty()) {
        const auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
                                deadline - std::chrono::steady_clock::now()).count();
        if (remain <= 0) return 0;

        std::uint8_t buf[512];
        impl_->ctx->usb_read_timeout = static_cast<int>(std::max<std::int64_t>(1, remain));
        const int got = ftdi_read_data(impl_->ctx, buf, static_cast<int>(sizeof(buf)));
        if (got < 0) return 0;              // 出错/断连
        if (got == 0) continue;

        for (int i = 0; i < got; ++i) {
            const std::uint8_t b = buf[i];
            if (b == kDelimiter) {
                impl_->inFrame = !impl_->inFrame;   // 定界符：帧内/帧外切换
                impl_->pending.push_back(b);
            } else if (b == kInjectedStatus && !impl_->inFrame) {
                continue;                            // 帧外状态字节：丢弃
            } else {
                impl_->pending.push_back(b);
            }
        }
    }

    const std::size_t n = std::min(size, impl_->pending.size());
    for (std::size_t i = 0; i < n; ++i) {
        data[i] = impl_->pending.front();
        impl_->pending.pop_front();
    }
    return n;
}

bool FtdiTransport::Write(const std::uint8_t* data, std::size_t size, int timeoutMs)
{
    if (!impl_->ctx || !data || size == 0) return false;

    impl_->ctx->usb_write_timeout = std::max(1, timeoutMs);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(1, timeoutMs));
    std::size_t written = 0;
    while (written < size) {
        const int n = ftdi_write_data(impl_->ctx,
                                      const_cast<std::uint8_t*>(data + written),
                                      static_cast<int>(size - written));
        if (n < 0) return false;
        written += static_cast<std::size_t>(n);
        if (std::chrono::steady_clock::now() >= deadline) break;
    }
    return written == size;
}

void FtdiTransport::DiscardInput()
{
    impl_->pending.clear();
    impl_->inFrame = false;
    if (impl_->ctx) ftdi_usb_purge_rx_buffer(impl_->ctx);
}

bool FtdiTransport::IsSupported() { return true; }

bool FtdiTransport::IsDevicePresent(std::uint16_t vendorId, std::uint16_t productId)
{
    ftdi_context* ctx = ftdi_new();
    if (!ctx) return false;
    struct ftdi_device_list* list = nullptr;
    const int n = ftdi_usb_find_all(ctx, &list, vendorId, productId);
    if (list) ftdi_list_free(&list);
    ftdi_free(ctx);
    return n > 0;
}

#endif  // SIGFLOW_HAVE_LIBFTDI

} // namespace debug
} // namespace sigflow
