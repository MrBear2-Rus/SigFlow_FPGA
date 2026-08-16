#include "PipeTransport.h"

#include <cstring>

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

PipeTransport::PipeTransport(std::string pipeName, bool server)
    : pipeName_(std::move(pipeName)), server_(server)
{
}

PipeTransport::~PipeTransport()
{
    Close();
}

bool PipeTransport::Open()
{
    if (hPipe_ != INVALID_HANDLE_VALUE) return true;
    return server_ ? OpenServer() : OpenClient();
}

bool PipeTransport::OpenServer()
{
    const std::wstring wide = ToWide(pipeName_);
    hPipe_ = CreateNamedPipeW(wide.c_str(),
                              PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                              1, 65536, 65536, 0, nullptr);
    if (hPipe_ == INVALID_HANDLE_VALUE) return false;

    // 服务端在后台线程等待客户端连接；CreateFileW 返回即代表连通。
    connectThread_ = std::thread(&PipeTransport::ServerConnectLoop, hPipe_);
    return true;
}

void PipeTransport::ServerConnectLoop(HANDLE hPipe)
{
    ConnectNamedPipe(hPipe, nullptr);
    // 结果不在此处判断：客户端成功打开即代表连接建立，
    // 连接失败会由后续 Read/Write 的断连错误体现。
}

bool PipeTransport::OpenClient()
{
    const std::wstring wide = ToWide(pipeName_);
    for (int attempt = 0; attempt < 50; ++attempt) {
        hPipe_ = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (hPipe_ != INVALID_HANDLE_VALUE) return true;
        if (GetLastError() != ERROR_PIPE_BUSY) return false;
        if (!WaitNamedPipeW(wide.c_str(), 100)) return false;
    }
    return false;
}

void PipeTransport::Close()
{
    if (hPipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(hPipe_);
        hPipe_ = INVALID_HANDLE_VALUE;
    }
    if (connectThread_.joinable()) connectThread_.join();
}

std::size_t PipeTransport::Read(std::uint8_t* data, std::size_t size, int timeoutMs)
{
    if (hPipe_ == INVALID_HANDLE_VALUE || size == 0 || timeoutMs < 0) return 0;

    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return 0;

    OVERLAPPED ov = {};
    ov.hEvent = event;
    DWORD got = 0;
    BOOL ok = ReadFile(hPipe_, data, static_cast<DWORD>(size), &got, &ov);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();

    if (!ok && error == ERROR_IO_PENDING) {
        const DWORD wait = WaitForSingleObject(event, static_cast<DWORD>(timeoutMs));
        if (wait == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(hPipe_, &ov, &got, FALSE);
            error = ok ? ERROR_SUCCESS : GetLastError();
        } else {
            // 超时边界上的完成竞态：先取消，再确认是否实际读到了数据，
            // 避免把恰好到达的字节当作超时丢弃。
            CancelIoEx(hPipe_, &ov);
            WaitForSingleObject(event, INFINITE);
            if (GetOverlappedResult(hPipe_, &ov, &got, FALSE)) {
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

bool PipeTransport::Write(const std::uint8_t* data, std::size_t size, int timeoutMs)
{
    if (hPipe_ == INVALID_HANDLE_VALUE || size == 0 || timeoutMs < 0) return false;

    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return false;

    OVERLAPPED ov = {};
    ov.hEvent = event;
    DWORD written = 0;
    BOOL ok = WriteFile(hPipe_, data, static_cast<DWORD>(size), &written, &ov);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();

    if (!ok && error == ERROR_IO_PENDING) {
        const DWORD wait = WaitForSingleObject(event, static_cast<DWORD>(timeoutMs));
        if (wait == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(hPipe_, &ov, &written, FALSE);
            error = ok ? ERROR_SUCCESS : GetLastError();
        } else {
            CancelIoEx(hPipe_, &ov);
            WaitForSingleObject(event, INFINITE);
            if (GetOverlappedResult(hPipe_, &ov, &written, FALSE) &&
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

bool PipeTransport::CreatePair(std::string pipeName,
                               std::unique_ptr<PipeTransport>& hostEnd,
                               std::unique_ptr<PipeTransport>& deviceEnd,
                               std::string& error)
{
    const std::string full = "\\\\.\\pipe\\" + std::move(pipeName);

    hostEnd = std::make_unique<PipeTransport>(full, false);
    deviceEnd = std::make_unique<PipeTransport>(full, true);

    if (!deviceEnd->Open()) {
        error = "server pipe create failed, lastError=" + std::to_string(GetLastError());
        return false;
    }
    if (!hostEnd->Open()) {
        error = "client pipe connect failed, lastError=" + std::to_string(GetLastError());
        deviceEnd->Close();
        return false;
    }

    if (deviceEnd->connectThread_.joinable()) deviceEnd->connectThread_.join();
    return true;
}

} // namespace debug
} // namespace sigflow
