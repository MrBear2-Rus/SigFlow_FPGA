#include "LocalPipe.h"

#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <thread>
#else
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
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
    std::wstring wide(static_cast<std::size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), len);
    return wide;
}

#else

// 命名端点对外的路径：已是绝对路径则原样使用，否则放到 /tmp 下。
std::string PipePath(const std::string& name)
{
    if (!name.empty() && name[0] == '/') return name;
    return "/tmp/sigflow_pipe_" + name;
}

#endif

} // namespace

struct LocalPipe::Impl {
    Impl(std::string pipeName, bool isServer)
        : name(std::move(pipeName)), server(isServer)
    {
    }

    ~Impl() { Close(); }

    std::string name;    // Win：完整 \\?\pipe\...（UTF-8）；POSIX：socket 路径
    bool server = false;

#if defined(_WIN32)
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    std::thread connectThread;
#else
    int fd = -1;
#endif

    bool Open();
    void Close();
    bool IsOpen() const;
    std::size_t Read(std::uint8_t* data, std::size_t size, int timeoutMs);
    bool Write(const std::uint8_t* data, std::size_t size, int timeoutMs);

#if defined(_WIN32)
    bool OpenServer();
    bool OpenClient();
    static void ServerConnectLoop(HANDLE hPipe);
#endif
};

#if defined(_WIN32)

bool LocalPipe::Impl::Open()
{
    if (hPipe != INVALID_HANDLE_VALUE) return true;
    return server ? OpenServer() : OpenClient();
}

bool LocalPipe::Impl::OpenServer()
{
    const std::wstring wide = ToWide(name);
    hPipe = CreateNamedPipeW(wide.c_str(),
                             PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                             1, 65536, 65536, 0, nullptr);
    if (hPipe == INVALID_HANDLE_VALUE) return false;

    // 服务端在后台线程等待客户端连接；CreateFileW 返回即代表连通。
    connectThread = std::thread(&Impl::ServerConnectLoop, hPipe);
    return true;
}

void LocalPipe::Impl::ServerConnectLoop(HANDLE hPipe)
{
    ConnectNamedPipe(hPipe, nullptr);
    // 结果不在此处判断：客户端成功打开即代表连接建立，
    // 连接失败会由后续 Read/Write 的断连错误体现。
}

bool LocalPipe::Impl::OpenClient()
{
    const std::wstring wide = ToWide(name);
    for (int attempt = 0; attempt < 50; ++attempt) {
        hPipe = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (hPipe != INVALID_HANDLE_VALUE) return true;
        if (GetLastError() != ERROR_PIPE_BUSY) return false;
        if (!WaitNamedPipeW(wide.c_str(), 100)) return false;
    }
    return false;
}

void LocalPipe::Impl::Close()
{
    if (hPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(hPipe);
        hPipe = INVALID_HANDLE_VALUE;
    }
    if (connectThread.joinable()) connectThread.join();
}

bool LocalPipe::Impl::IsOpen() const
{
    return hPipe != INVALID_HANDLE_VALUE;
}

std::size_t LocalPipe::Impl::Read(std::uint8_t* data, std::size_t size, int timeoutMs)
{
    if (hPipe == INVALID_HANDLE_VALUE || size == 0 || timeoutMs < 0) return 0;

    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return 0;

    OVERLAPPED ov = {};
    ov.hEvent = event;
    DWORD got = 0;
    BOOL ok = ReadFile(hPipe, data, static_cast<DWORD>(size), &got, &ov);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();

    if (!ok && error == ERROR_IO_PENDING) {
        const DWORD wait = WaitForSingleObject(event, static_cast<DWORD>(timeoutMs));
        if (wait == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(hPipe, &ov, &got, FALSE);
            error = ok ? ERROR_SUCCESS : GetLastError();
        } else {
            // 超时边界上的完成竞态：先取消，再确认是否实际读到了数据，
            // 避免把恰好到达的字节当作超时丢弃。
            CancelIoEx(hPipe, &ov);
            WaitForSingleObject(event, INFINITE);
            if (GetOverlappedResult(hPipe, &ov, &got, FALSE)) {
                CloseHandle(event);
                return got;
            }
            CloseHandle(event);
            return 0;  // 真正超时
        }
    }

    CloseHandle(event);
    if (ok) return got;

    if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED ||
        error == ERROR_NO_DATA) {
        Close();  // 对端断开
    }
    return 0;
}

bool LocalPipe::Impl::Write(const std::uint8_t* data, std::size_t size, int timeoutMs)
{
    if (hPipe == INVALID_HANDLE_VALUE || size == 0 || timeoutMs < 0) return false;

    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return false;

    OVERLAPPED ov = {};
    ov.hEvent = event;
    DWORD written = 0;
    BOOL ok = WriteFile(hPipe, data, static_cast<DWORD>(size), &written, &ov);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();

    if (!ok && error == ERROR_IO_PENDING) {
        const DWORD wait = WaitForSingleObject(event, static_cast<DWORD>(timeoutMs));
        if (wait == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(hPipe, &ov, &written, FALSE);
            error = ok ? ERROR_SUCCESS : GetLastError();
        } else {
            CancelIoEx(hPipe, &ov);
            WaitForSingleObject(event, INFINITE);
            if (GetOverlappedResult(hPipe, &ov, &written, FALSE) &&
                written == static_cast<DWORD>(size)) {
                CloseHandle(event);
                return true;
            }
            CloseHandle(event);
            return false;  // 写超时
        }
    }

    CloseHandle(event);
    if (ok && written == static_cast<DWORD>(size)) return true;

    if (!ok && (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED ||
                error == ERROR_NO_DATA)) {
        Close();
    }
    return false;
}

#else  // !defined(_WIN32)

bool LocalPipe::Impl::Open()
{
    if (fd >= 0) return true;

    const std::string path = PipePath(name);
    if (path.size() >= sizeof(sockaddr_un::sun_path)) return false;

    if (server) {
        const int listenFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listenFd < 0) return false;

        sockaddr_un addr = {};
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, path.c_str(), path.size());
        ::unlink(path.c_str());
        if (::bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(listenFd);
            return false;
        }
        if (::listen(listenFd, 1) != 0) {
            ::close(listenFd);
            ::unlink(path.c_str());
            return false;
        }

        pollfd pfd = {};
        pfd.fd = listenFd;
        pfd.events = POLLIN;
        int rc = 0;
        do {
            rc = ::poll(&pfd, 1, 30000);
        } while (rc < 0 && errno == EINTR);

        int conn = -1;
        if (rc > 0) {
            do {
                conn = ::accept(listenFd, nullptr, nullptr);
            } while (conn < 0 && errno == EINTR);
        }
        ::close(listenFd);
        ::unlink(path.c_str());
        if (conn < 0) return false;
        fd = conn;
        return true;
    }

    const int clientFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (clientFd < 0) return false;

    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size());
    if (::connect(clientFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(clientFd);
        return false;
    }
    fd = clientFd;
    return true;
}

void LocalPipe::Impl::Close()
{
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

bool LocalPipe::Impl::IsOpen() const
{
    return fd >= 0;
}

std::size_t LocalPipe::Impl::Read(std::uint8_t* data, std::size_t size, int timeoutMs)
{
    if (fd < 0 || size == 0 || timeoutMs < 0) return 0;

    pollfd pfd = {};
    pfd.fd = fd;
    pfd.events = POLLIN;
    int rc = 0;
    do {
        rc = ::poll(&pfd, 1, timeoutMs);
    } while (rc < 0 && errno == EINTR);

    if (rc == 0) return 0;  // 超时
    if (rc < 0) {
        Close();
        return 0;
    }

    ssize_t got = 0;
    do {
        got = ::read(fd, data, size);
    } while (got < 0 && errno == EINTR);
    if (got <= 0) {
        Close();  // 对端断开
        return 0;
    }
    return static_cast<std::size_t>(got);
}

bool LocalPipe::Impl::Write(const std::uint8_t* data, std::size_t size, int timeoutMs)
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
        if (rc <= 0) {
            if (rc < 0) Close();
            return false;  // 写超时
        }

        ssize_t n = 0;
        do {
            n = ::write(fd, data + written, size - written);
        } while (n < 0 && errno == EINTR);
        if (n <= 0) {
            Close();
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    return true;
}

#endif  // _WIN32

LocalPipe::LocalPipe(std::string name, bool server)
    : m_impl(std::make_unique<Impl>(std::move(name), server))
{
}

LocalPipe::~LocalPipe() = default;

bool LocalPipe::Open() { return m_impl->Open(); }
void LocalPipe::Close() { m_impl->Close(); }
bool LocalPipe::IsOpen() const { return m_impl->IsOpen(); }

std::size_t LocalPipe::Read(std::uint8_t* data, std::size_t size, int timeoutMs)
{
    return m_impl->Read(data, size, timeoutMs);
}

bool LocalPipe::Write(const std::uint8_t* data, std::size_t size, int timeoutMs)
{
    return m_impl->Write(data, size, timeoutMs);
}

bool LocalPipe::CreatePair(std::string name,
                           std::unique_ptr<LocalPipe>& hostEnd,
                           std::unique_ptr<LocalPipe>& deviceEnd,
                           std::string& error)
{
#if defined(_WIN32)
    const std::string full = "\\\\.\\pipe\\" + std::move(name);

    hostEnd = std::make_unique<LocalPipe>(full, false);
    deviceEnd = std::make_unique<LocalPipe>(full, true);

    if (!deviceEnd->Open()) {
        error = "server pipe create failed, lastError=" + std::to_string(GetLastError());
        return false;
    }
    if (!hostEnd->Open()) {
        error = "client pipe connect failed, lastError=" + std::to_string(GetLastError());
        deviceEnd->Close();
        return false;
    }

    if (deviceEnd->m_impl->connectThread.joinable()) {
        deviceEnd->m_impl->connectThread.join();
    }
    return true;
#else
    int fds[2] = { -1, -1 };
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        error = "socketpair failed, errno=" + std::to_string(errno);
        return false;
    }

    hostEnd = std::make_unique<LocalPipe>(name, false);
    deviceEnd = std::make_unique<LocalPipe>(std::move(name), true);
    hostEnd->m_impl->fd = fds[0];
    deviceEnd->m_impl->fd = fds[1];
    return true;
#endif
}

} // namespace sigflow::platform
