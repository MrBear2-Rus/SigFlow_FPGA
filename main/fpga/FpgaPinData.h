#pragma once

#include <wx/string.h>
#include <vector>

// ==================== 封装引脚 ====================
struct PackagePin {
    int pinNumber = 0;          // 1-88
    int bank = 0;               // Bank 编号 (0, 1, 2)
    wxString ioLocation;        // IO 位置: e.g. "IOT10A", "IOL5B", "IOR3C"
    bool available = true;      // false = 电源/地/JTAG/保留
    wxString reservedReason;    // 不可用原因
    wxString defaultIOType;     // 默认 IO_TYPE ("LVCMOS33")
};

// ==================== 板级资源别名 ====================
struct BoardResource {
    wxString alias;             // e.g. "LED1", "BTN1", "UART_TX", "CLK"
    int packagePin = 0;         // 对应的封装引脚号
    wxString description;       // 简要描述
    wxString resourceType;      // "LED", "BUTTON", "UART", "CLK", "SPI", "JTAG", "USB", "POWER", "RGB"
};

// ==================== 引脚数据库 ====================
struct FpgaPinDatabase {
    wxString device;            // "GW1NR-LV9QN88PC6/I5"
    wxString packageName;       // "QFN88"
    std::vector<PackagePin> pins;
    std::vector<BoardResource> boardResources;

    // 查询接口
    const PackagePin* FindPin(int pinNumber) const;
    const BoardResource* FindResourceByPin(int pinNumber) const;
    std::vector<const BoardResource*> FindResourcesByType(const wxString& type) const;
    std::vector<int> GetAvailablePins() const;
    bool IsPinAvailable(int pinNumber) const;
};

// ==================== 工厂函数 ====================
const FpgaPinDatabase& GetQFN88PinDatabase();

// ==================== 已知的 IO_TYPE 值 ====================
const std::vector<wxString>& GetKnownIOTypes();

// ==================== 引脚方向枚举 ====================
enum class FpgaPortDirection {
    Input,
    Output,
    InOut
};

inline wxString FpgaPortDirectionToString(FpgaPortDirection dir) {
    switch (dir) {
    case FpgaPortDirection::Input:  return "input";
    case FpgaPortDirection::Output: return "output";
    case FpgaPortDirection::InOut:  return "inout";
    }
    return "unknown";
}

inline FpgaPortDirection FpgaPortDirectionFromString(const wxString& s) {
    if (s == "input" || s == "in" || s == "In")  return FpgaPortDirection::Input;
    if (s == "output" || s == "out" || s == "Out") return FpgaPortDirection::Output;
    if (s == "inout" || s == "InOut") return FpgaPortDirection::InOut;
    return FpgaPortDirection::Input;
}
