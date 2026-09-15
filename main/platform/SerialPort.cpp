#include "SerialPort.h"

#include <algorithm>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace sigflow::platform {

namespace {

#if defined(_WIN32)

std::wstring ToWide(const std::string& utf8)
{
    if (utf8.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), len);
    wide.resize(static_cast<std::size_t>(len - 1));
    return wide;
}

#else

// 常见波特率 -> termios speed 常量；不在表内的返回 false（保留驱动默认）。
bool ToSpeedConstant(std::uint32_t baud, speed_t& out)
{
    switch (baud) {
#ifdef B110
    case 110: out = B110; return true;
#endif
#ifdef B300
    case 300: out = B300; return true;
#endif
#ifdef B600
    case 600: out = B600; return true;
#endif
#ifdef B1200
    case 1200: out = B1200; return true;
#endif
#ifdef B2400
    case 2400: out = B2400; return true;
#endif
#ifdef B4800
    case 4800: out = B4800; return true;
#endif
#ifdef B9600
    case 9600: out = B9600; return true;
#endif
#ifdef B19200
    case 19200: out = B19200; return true;
#endif
#ifdef B38400
    case 38400: out = B38400; return true;
#endif
#ifdef B57600
    case 57600: out = B57600; return true;
#endif
#ifdef B115200
    case 115200: out = B115200; return true;
#endif
#ifdef B230400
    case 230400: out = B230400; return true;
#endif
#ifdef B460800
    case 460800: out = B460800; return true;
#endif
#ifdef B500000
    case 500000: out = B500000; return true;
#endif
#ifdef B576000
    case 576000: out = B576000; return true;
#endif
#ifdef B921600
    case 921600: out = B921600; return true;
#endif
#ifdef B1000000
    case 1000000: out = B1000000; return true;
#endif
#ifdef B1152000
    case 1152000: out = B1152000; return true;
#endif
#ifdef B1500000
    case 1500000: out = B1500000; return true;
#endif
#ifdef B2000000
    case 2000000: out = B2000000; return true;
#endif
#ifdef B2500000
    case 2500000: out = B2500000; return true;
#endif
#ifdef B3000000
    case 3000000: out = B3000000; return true;
#endif
    default: return false;
    }
}

#endif

} // namespace

struct SerialPort::Impl {
    Impl(std::string name, std::uint32_t baudRate)
        : portName(std::move(name)), baud(baudRate)
    {
    }

    ~Impl() { Close(); }

    std::string portName;   // "COM3"（打开时补 \\.\ 前缀）
    std::uint32_t baud = 921600;

#if defined(_WIN32)
    HANDLE hCom = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif

    bool Open();
    void Close();
    bool IsOpen() const;
    std::size_t Read(std::uint8_t* data, std::size_t size, int timeoutMs);
    bool Write(const std::uint8_t* data, std::size_t size, int timeoutMs);
    void DiscardInput();
    bool SetBaudRate(std::uint32_t baudRate);
};

#if defined(_WIN32)

bool SerialPort::Impl::Open()
{
    if (hCom != INVALID_HANDLE_VALUE) return true;
    const std::string path = "\\\\.\\" + portName;
    const std::wstring wide = ToWide(path);
    hCom = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (hCom == INVALID_HANDLE_VALUE) return false;

    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(hCom, &dcb)) {
        Close();
        return false;
    }
    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    if (!SetCommState(hCom, &dcb)) {
        Close();
        return false;
    }

    // 超时由重叠 I/O 等待控制，OS 层关闭读写超时。
    COMMTIMEOUTS timeouts = {};
    SetCommTimeouts(hCom, &timeouts);
    PurgeComm(hCom, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return true;
}

void SerialPort::Impl::Close()
{
    if (hCom != INVALID_HANDLE_VALUE) {
        CloseHandle(hCom);
        hCom = INVALID_HANDLE_VALUE;
    }
}

bool SerialPort::Impl::IsOpen() const
{
    return hCom != INVALID_HANDLE_VALUE;
}

std::size_t SerialPort::Impl::Read(std::uint8_t* data, std::size_t size, int timeoutMs)
{
    if (hCom == INVALID_HANDLE_VALUE || size == 0 || timeoutMs < 0) return 0;

    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return 0;

    DWORD commErrors = 0;
    COMSTAT commStatus = {};
    if (!ClearCommError(hCom, &commErrors, &commStatus)) {
        CloseHandle(event);
        Close();
        return 0;
    }
    const std::size_t queued = static_cast<std::size_t>(commStatus.cbInQue);
    const std::size_t requestSize = std::max<std::size_t>(
        1, std::min<std::size_t>(size, queued));

    OVERLAPPED ov = {};
    ov.hEvent = event;
    DWORD got = 0;
    BOOL ok = ReadFile(hCom, data, static_cast<DWORD>(requestSize), &got, &ov);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && error == ERROR_IO_PENDING) {
        const DWORD wait = WaitForSingleObject(event, static_cast<DWORD>(timeoutMs));
        if (wait == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(hCom, &ov, &got, FALSE);
            error = ok ? ERROR_SUCCESS : GetLastError();
        } else {
            CancelIoEx(hCom, &ov);
            WaitForSingleObject(event, INFINITE);
            if (GetOverlappedResult(hCom, &ov, &got, FALSE)) {
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

bool SerialPort::Impl::Write(const std::uint8_t* data, std::size_t size, int timeoutMs)
{
    if (hCom == INVALID_HANDLE_VALUE || size == 0 || timeoutMs < 0) return false;

    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return false;
    OVERLAPPED ov = {};
    ov.hEvent = event;
    DWORD written = 0;
    BOOL ok = WriteFile(hCom, data, static_cast<DWORD>(size), &written, &ov);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && error == ERROR_IO_PENDING) {
        const DWORD wait = WaitForSingleObject(event, static_cast<DWORD>(timeoutMs));
        if (wait == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(hCom, &ov, &written, FALSE);
            error = ok ? ERROR_SUCCESS : GetLastError();
        } else {
            CancelIoEx(hCom, &ov);
            WaitForSingleObject(event, INFINITE);
            const bool completed =
                GetOverlappedResult(hCom, &ov, &written, FALSE) &&
                written == static_cast<DWORD>(size);
            CloseHandle(event);
            return completed;
        }
    }
    CloseHandle(event);
    return ok && written == static_cast<DWORD>(size);
}

void SerialPort::Impl::DiscardInput()
{
    if (hCom != INVALID_HANDLE_VALUE) {
        PurgeComm(hCom, PURGE_RXCLEAR);
    }
}

bool SerialPort::Impl::SetBaudRate(std::uint32_t baudRate)
{
    if (!SerialPort::IsSupportedBaud(baudRate)) return false;
    baud = baudRate;
    if (hCom == INVALID_HANDLE_VALUE) return true;  // 未打开：记住即可
    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(hCom, &dcb)) return false;
    dcb.BaudRate = baud;
    return SetCommState(hCom, &dcb) != FALSE;
}

#else  // !defined(_WIN32)

bool SerialPort::Impl::Open()
{
    if (fd >= 0) return true;
    fd = ::open(portName.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return false;

    termios tty = {};
    if (::tcgetattr(fd, &tty) != 0) {
        Close();
        return false;
    }
    ::cfmakeraw(&tty);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;      // 8 数据位
    tty.c_cflag &= ~PARENB;  // 无校验
    tty.c_cflag &= ~CSTOPB;  // 1 停止位
#ifdef CRTSCTS
    tty.c_cflag &= ~CRTSCTS; // 无硬件流控
#endif
#ifdef IXON
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);  // 无软件流控
#endif
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    speed_t speed = B0;
    if (ToSpeedConstant(baud, speed)) {
        ::cfsetispeed(&tty, speed);
        ::cfsetospeed(&tty, speed);
    }
    if (::tcsetattr(fd, TCSANOW, &tty) != 0) {
        Close();
        return false;
    }
    ::tcflush(fd, TCIOFLUSH);
    return true;
}

void SerialPort::Impl::Close()
{
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

bool SerialPort::Impl::IsOpen() const
{
    return fd >= 0;
}

std::size_t SerialPort::Impl::Read(std::uint8_t* data, std::size_t size, int timeoutMs)
{
    if (fd < 0 || size == 0 || timeoutMs < 0) return 0;

    pollfd pfd = {};
    pfd.fd = fd;
    pfd.events = POLLIN;
    int rc = 0;
    do {
        rc = ::poll(&pfd, 1, timeoutMs);
    } while (rc < 0 && errno == EINTR);
    if (rc <= 0) return 0;  // 超时

    ssize_t got = 0;
    do {
        got = ::read(fd, data, size);
    } while (got < 0 && errno == EINTR);
    if (got < 0) {
        if (errno == EIO || errno == ENXIO || errno == EBADF) Close();
        return 0;
    }
    return static_cast<std::size_t>(got);
}

bool SerialPort::Impl::Write(const std::uint8_t* data, std::size_t size, int timeoutMs)
{
    if (fd < 0 || size == 0 || timeoutMs < 0) return false;

    std::size_t written = 0;
    while (written < size) {
        pollfd pfd = {};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        int rc = 0;
        do {
            rc = ::poll(&pfd, 1, timeoutMs);
        } while (rc < 0 && errno == EINTR);
        if (rc <= 0) return false;  // 写超时

        ssize_t n = 0;
        do {
            n = ::write(fd, data + written, size - written);
        } while (n < 0 && errno == EINTR);
        if (n < 0) {
            if (errno == EIO || errno == ENXIO || errno == EBADF) Close();
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    return true;
}

void SerialPort::Impl::DiscardInput()
{
    if (fd >= 0) ::tcflush(fd, TCIFLUSH);
}

bool SerialPort::Impl::SetBaudRate(std::uint32_t baudRate)
{
    if (!SerialPort::IsSupportedBaud(baudRate)) return false;
    baud = baudRate;
    if (fd < 0) return true;  // 未打开：记住即可
    termios tty = {};
    if (::tcgetattr(fd, &tty) != 0) return false;
    speed_t speed = B0;
    if (ToSpeedConstant(baud, speed)) {
        ::cfsetispeed(&tty, speed);
        ::cfsetospeed(&tty, speed);
    }
    return ::tcsetattr(fd, TCSANOW, &tty) == 0;
}

#endif  // _WIN32

SerialPort::SerialPort(std::string portName, std::uint32_t baud)
    : m_impl(std::make_unique<Impl>(std::move(portName), baud))
{
}

SerialPort::~SerialPort() = default;

bool SerialPort::Open() { return m_impl->Open(); }
void SerialPort::Close() { m_impl->Close(); }
bool SerialPort::IsOpen() const { return m_impl->IsOpen(); }

std::size_t SerialPort::Read(std::uint8_t* data, std::size_t size, int timeoutMs)
{
    return m_impl->Read(data, size, timeoutMs);
}

bool SerialPort::Write(const std::uint8_t* data, std::size_t size, int timeoutMs)
{
    return m_impl->Write(data, size, timeoutMs);
}

void SerialPort::DiscardInput() { m_impl->DiscardInput(); }

bool SerialPort::SetBaudRate(std::uint32_t baud)
{
    return m_impl->SetBaudRate(baud);
}

const std::string& SerialPort::PortName() const { return m_impl->portName; }

bool SerialPort::IsSupportedBaud(std::uint32_t baud)
{
    // 覆盖 110 ~ 3M 常见范围；驱动对任意值都有容忍度，这里只做合理性检查。
    return baud >= 110 && baud <= 3000000;
}

} // namespace sigflow::platform
