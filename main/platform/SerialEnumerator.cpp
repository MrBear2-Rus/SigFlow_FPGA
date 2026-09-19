#include "SerialEnumerator.h"

#if defined(_WIN32)
#include <windows.h>
#include <setupapi.h>
#else
#include <filesystem>
#include <cstdio>
#include <fstream>
#endif

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace sigflow::platform {

#if defined(_WIN32)

namespace {

// GUID_DEVINTERFACE_COMPORT
const GUID kComPortGuid = { 0x86E0D1E0, 0x8089, 0x11D0,
                            { 0x9C, 0xE4, 0x08, 0x00, 0x3E, 0x30, 0x1F, 0x73 } };

std::wstring ToWide(const std::string& utf8)
{
    if (utf8.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), len);
    return wide;
}

std::string FromWide(const std::wstring& wide)
{
    if (wide.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0,
                                        nullptr, nullptr);
    if (len <= 0) return {};
    std::string utf8(static_cast<std::size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, utf8.data(), len, nullptr, nullptr);
    return utf8;
}

// 从设备路径尾部提取 "COMx"（路径形如 ...\usbser000\COM3）。
std::string ComNameFromPath(const std::wstring& path)
{
    const std::string utf8 = FromWide(path);
    std::size_t pos = utf8.rfind("COM");
    if (pos == std::string::npos) return {};
    std::size_t digits = pos + 3;
    while (digits < utf8.size() &&
           std::isdigit(static_cast<unsigned char>(utf8[digits]))) {
        ++digits;
    }
    if (digits == pos + 3) return {};
    return utf8.substr(pos, digits - pos);
}

std::vector<std::string> RegistryComPorts()
{
    std::vector<std::string> names;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
        return names;
    }
    for (DWORD index = 0;; ++index) {
        wchar_t valueName[256] = {};
        DWORD valueNameSize = 256;
        wchar_t valueData[64] = {};
        DWORD valueDataSize = sizeof(valueData);
        const LSTATUS rc = RegEnumValueW(key, index, valueName, &valueNameSize, nullptr,
                                         nullptr, reinterpret_cast<BYTE*>(valueData),
                                         &valueDataSize);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS) continue;
        const std::string com = FromWide(valueData);
        if (com.rfind("COM", 0) == 0) names.push_back(com);
    }
    RegCloseKey(key);
    return names;
}

} // namespace

std::vector<SerialPortInfo> EnumerateSerialPorts()
{
    std::vector<SerialPortInfo> result;
    const std::vector<std::string> registryPorts = RegistryComPorts();

    // SetupAPI：设备路径 -> 友好名称
    std::vector<std::pair<std::string, std::string>> pathFriendly;
    HDEVINFO devInfo = SetupDiGetClassDevsW(&kComPortGuid, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo != INVALID_HANDLE_VALUE) {
        SP_DEVICE_INTERFACE_DATA ifData = {};
        ifData.cbSize = sizeof(ifData);
        for (DWORD index = 0;
             SetupDiEnumDeviceInterfaces(devInfo, nullptr, &kComPortGuid, index, &ifData);
             ++index) {
            DWORD required = 0;
            SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &required,
                                             nullptr);
            if (required == 0) continue;
            std::vector<BYTE> buffer(required);
            auto* detail =
                reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            SP_DEVINFO_DATA devInfoData = {};
            devInfoData.cbSize = sizeof(devInfoData);
            if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, required,
                                                  nullptr, &devInfoData)) {
                continue;
            }
            const std::string com = ComNameFromPath(detail->DevicePath);
            if (com.empty()) continue;
            wchar_t friendly[256] = {};
            DWORD friendlySize = 0;
            if (SetupDiGetDeviceRegistryPropertyW(devInfo, &devInfoData,
                                                  SPDRP_FRIENDLYNAME, nullptr,
                                                  reinterpret_cast<BYTE*>(friendly),
                                                  sizeof(friendly), &friendlySize) &&
                friendlySize > 0) {
                pathFriendly.emplace_back(com, FromWide(friendly));
            }
        }
        SetupDiDestroyDeviceInfoList(devInfo);
    }

    const auto friendlyFor = [&pathFriendly](const std::string& com) -> std::string {
        for (const auto& item : pathFriendly) {
            if (item.first == com) return item.second;
        }
        return {};
    };

    // 以注册表顺序输出；取不到友好名称时回退为端口名。
    for (const std::string& com : registryPorts) {
        SerialPortInfo info;
        info.name = com;
        info.friendlyName = friendlyFor(com);
        if (info.friendlyName.empty()) info.friendlyName = com;
        result.push_back(std::move(info));
    }
    return result;
}

#else  // !defined(_WIN32)

namespace {

// idVendor / idProduct / serial 是纯文本 sysfs 属性，去掉行尾空白。
std::string ReadSysfsText(const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in) return {};
    std::string value;
    std::getline(in, value);
    while (!value.empty() &&
           (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' ||
            value.back() == '\t')) {
        value.pop_back();
    }
    return value;
}

// 读取 USB 串口的归属信息。实测层级（Tang Nano 9K 板载 BL702 / FT2232）：
//   .../3-8:1.1/ttyUSB1   ← 起点：/sys/class/tty/ttyUSB1/device 解析后的目标
//   .../3-8:1.1           ← bInterfaceNumber（**文本**属性，内容形如 "1\n"）
//   .../3-8               ← idVendor / idProduct / serial
// 不同内核层级可能略有差异，因此不写死层数，向上搜索最多 3 层。
void FillUsbInfo(SerialPortInfo& info)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path node = fs::weakly_canonical(
        fs::path("/sys/class/tty") / fs::path(info.name).filename() / "device", ec);
    if (ec || node.empty()) return;

    for (int up = 0; up < 3 && !node.empty(); ++up) {
        if (info.interfaceNumber < 0) {
            const std::string raw = ReadSysfsText(node / "bInterfaceNumber");
            if (!raw.empty()) {
                // sysfs 属性是【文本】：内容形如 "1\n"。
                // 曾按二进制单字节读，'1' 会变成 0x31=49，通道判断会全错。
                try {
                    info.interfaceNumber = std::stoi(raw);
                } catch (...) {
                }
            }
        }
        if (info.vendorId.empty())  info.vendorId  = ReadSysfsText(node / "idVendor");
        if (info.productId.empty()) info.productId = ReadSysfsText(node / "idProduct");
        if (info.usbSerial.empty()) info.usbSerial = ReadSysfsText(node / "serial");
        if (info.interfaceNumber >= 0 && !info.vendorId.empty() && !info.productId.empty()) break;
        node = node.parent_path();
    }
}

} // namespace

std::vector<SerialPortInfo> EnumerateSerialPorts()
{
    namespace fs = std::filesystem;
    std::vector<SerialPortInfo> result;

    // 首选 /dev/serial/by-id：friendlyName=软链名，name=解析后的设备基名。
    std::error_code ec;
    const fs::path byId("/dev/serial/by-id");
    if (fs::exists(byId, ec)) {
        for (const auto& entry : fs::directory_iterator(byId, ec)) {
            if (ec) break;
            SerialPortInfo info;
            info.friendlyName = entry.path().filename().string();
            std::error_code readEc;
            fs::path target = fs::read_symlink(entry.path(), readEc);
            if (!readEc) {
                if (target.is_relative()) target = byId / target;
                // 必须保留**绝对路径**：上层会用这个名字直接 open()，
                // 若只取 filename()（如 "ttyUSB0"）就会变成相对当前工作目录打开，
                // 必然 ENOENT。udev 对几乎每个 USB 串口都建立 /dev/serial/by-id，
                // 所以这里正是 Linux 上的主分支，取错等于 Linux 串口功能整体不可用。
                info.name = target.lexically_normal().string();
            }
            if (info.name.empty()) info.name = info.friendlyName;
            FillUsbInfo(info);
            result.push_back(std::move(info));
        }
    }
    if (!result.empty()) return result;

    // 回退：/dev/ttyUSB*、/dev/ttyACM*。
    std::error_code devEc;
    const fs::path dev("/dev");
    if (fs::exists(dev, devEc)) {
        for (const auto& entry : fs::directory_iterator(dev, devEc)) {
            if (devEc) break;
            const std::string filename = entry.path().filename().string();
            if (filename.rfind("ttyUSB", 0) != 0 && filename.rfind("ttyACM", 0) != 0 &&
                filename.rfind("ttyS", 0) != 0 && filename.rfind("ttyAMA", 0) != 0 &&
                filename.rfind("ttyXRUSB", 0) != 0 && filename.rfind("rfcomm", 0) != 0) {
                continue;
            }
            SerialPortInfo info;
            info.name = entry.path().string();
            info.friendlyName = info.name;
            FillUsbInfo(info);
            result.push_back(std::move(info));
        }
    }
    return result;
}

#endif  // _WIN32

} // namespace sigflow::platform
