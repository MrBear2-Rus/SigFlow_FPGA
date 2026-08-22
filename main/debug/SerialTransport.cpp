#include "SerialTransport.h"

#include <algorithm>

namespace sigflow {
namespace debug {

namespace {

std::wstring ToWide(const std::string& utf8)
{
    if (utf8.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), len);
    return wide;
}

} // namespace

SerialTransport::SerialTransport(std::string portName, std::uint32_t baud)
    : portName_(std::move(portName)), baud_(baud)
{
}

SerialTransport::~SerialTransport()
{
    Close();
}

bool SerialTransport::Open()
{
    if (hCom_ != INVALID_HANDLE_VALUE) return true;
    const std::string path = "\\\\.\\" + portName_;
    const std::wstring wide = ToWide(path);
    hCom_ = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (hCom_ == INVALID_HANDLE_VALUE) return false;

    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(hCom_, &dcb)) {
        Close();
        return false;
    }
    dcb.BaudRate = baud_;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    if (!SetCommState(hCom_, &dcb)) {
        Close();
        return false;
    }

    // 超时由重叠 I/O 等待控制，OS 层关闭读写超时。
    COMMTIMEOUTS timeouts = {};
    SetCommTimeouts(hCom_, &timeouts);
    PurgeComm(hCom_, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return true;
}

void SerialTransport::Close()
{
    if (hCom_ != INVALID_HANDLE_VALUE) {
        CloseHandle(hCom_);
        hCom_ = INVALID_HANDLE_VALUE;
    }
}

std::size_t SerialTransport::Read(std::uint8_t* data, std::size_t size, int timeoutMs)
{
    if (hCom_ == INVALID_HANDLE_VALUE || size == 0 || timeoutMs < 0) return 0;

    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return 0;
    OVERLAPPED ov = {};
    ov.hEvent = event;
    DWORD got = 0;
    BOOL ok = ReadFile(hCom_, data, static_cast<DWORD>(size), &got, &ov);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && error == ERROR_IO_PENDING) {
        const DWORD wait = WaitForSingleObject(event, static_cast<DWORD>(timeoutMs));
        if (wait == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(hCom_, &ov, &got, FALSE);
            error = ok ? ERROR_SUCCESS : GetLastError();
        } else {
            CancelIoEx(hCom_, &ov);
            WaitForSingleObject(event, INFINITE);
            if (GetOverlappedResult(hCom_, &ov, &got, FALSE)) {
                CloseHandle(event);
                return got;
            }
            CloseHandle(event);
            return 0;  // 超时
        }
    }
    CloseHandle(event);
    if (ok) return got;
    Close();  // 串口错误/拔出：标记关闭
    return 0;
}

bool SerialTransport::Write(const std::uint8_t* data, std::size_t size, int timeoutMs)
{
    if (hCom_ == INVALID_HANDLE_VALUE || size == 0 || timeoutMs < 0) return false;

    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return false;
    OVERLAPPED ov = {};
    ov.hEvent = event;
    DWORD written = 0;
    BOOL ok = WriteFile(hCom_, data, static_cast<DWORD>(size), &written, &ov);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && error == ERROR_IO_PENDING) {
        const DWORD wait = WaitForSingleObject(event, static_cast<DWORD>(timeoutMs));
        if (wait == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(hCom_, &ov, &written, FALSE);
            error = ok ? ERROR_SUCCESS : GetLastError();
        } else {
            CancelIoEx(hCom_, &ov);
            WaitForSingleObject(event, INFINITE);
            const bool completed =
                GetOverlappedResult(hCom_, &ov, &written, FALSE) &&
                written == static_cast<DWORD>(size);
            CloseHandle(event);
            return completed;
        }
    }
    CloseHandle(event);
    return ok && written == static_cast<DWORD>(size);
}

bool SerialTransport::SetBaudRate(std::uint32_t baud)
{
    if (!IsSupportedBaud(baud)) return false;
    baud_ = baud;
    if (hCom_ == INVALID_HANDLE_VALUE) return true;  // 未打开：记住即可
    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(hCom_, &dcb)) return false;
    dcb.BaudRate = baud;
    return SetCommState(hCom_, &dcb) != FALSE;
}

bool SerialTransport::IsSupportedBaud(std::uint32_t baud)
{
    // 覆盖 110 ~ 3M 常见范围；驱动对任意值都有容忍度，这里只做合理性检查。
    return baud >= 110 && baud <= 3000000;
}

} // namespace debug
} // namespace sigflow
