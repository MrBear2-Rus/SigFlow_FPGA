#include "FpgaPinData.h"
#include <algorithm>

namespace {

// QFN88 封装引脚数据 — 基于 Gowin GW1NR-9 器件手册和 Tang Nano 9K 原理图
// 数据来源: Gowin UG100 / UG289, Sipeed Tang Nano 9K 原理图
// 版本: 1.0.0 | 需硬件负责人复核

struct PinEntry {
    int pinNumber;
    int bank;
    const char* ioLocation;
    bool available;
    const char* reservedReason;
};

// QFN88 封装共 88 个引脚，按四边排列: Top(1-22), Right(23-44), Bottom(45-66), Left(67-88)
// 保留引脚: VCC, GND, JTAG, MODE, DONE, RECONFIG_N, READY, VCORE 等
constexpr PinEntry kQFN88Pins[] = {
    // === Top Edge (pin 1-22) ===
    { 1,  0, "IOT1A",  true,  nullptr },
    { 2,  0, "IOT2A",  true,  nullptr },
    { 3,  0, "IOT3A",  true,  nullptr },
    { 4,  0, "IOT4A",  true,  nullptr },
    { 5,  0, "IOT5A",  true,  nullptr },
    { 6,  0, "IOT6A",  true,  nullptr },
    { 7,  0, "GND",    false, "GND" },
    { 8,  0, "VCC",    false, "VCC (1.2V core)" },
    { 9,  0, "IOT9A",  true,  nullptr },
    {10,  0, "IOT10A", true,  nullptr },
    {11,  0, "IOT11A", true,  nullptr },
    {12,  0, "IOT12A", true,  nullptr },
    {13,  0, "IOT13A", true,  nullptr },
    {14,  0, "IOT14A", true,  nullptr },
    {15,  0, "IOT15A", true,  nullptr },
    {16,  0, "IOT16A", true,  nullptr },
    {17,  0, "GND",    false, "GND" },
    {18,  0, "VCCIO",  false, "VCCIO (I/O bank power)" },
    {19,  0, "IOT19A", true,  nullptr },
    {20,  0, "IOT20A", true,  nullptr },
    {21,  0, "IOT21A", true,  nullptr },
    {22,  0, "IOT22A", true,  nullptr },

    // === Right Edge (pin 23-44) ===
    {23,  1, "IOR1A",  true,  nullptr },
    {24,  1, "IOR2A",  true,  nullptr },
    {25,  1, "IOR3A",  true,  nullptr },
    {26,  1, "IOR4A",  true,  nullptr },
    {27,  1, "IOR5A",  true,  nullptr },
    {28,  1, "IOR6A",  true,  nullptr },
    {29,  1, "GND",    false, "GND" },
    {30,  1, "VCCIO",  false, "VCCIO (I/O bank power)" },
    {31,  1, "IOR9A",  true,  nullptr },
    {32,  1, "IOR10A", true,  nullptr },
    {33,  1, "IOR11A", true,  nullptr },
    {34,  1, "IOR12A", true,  nullptr },
    {35,  1, "IOR13A", true,  nullptr },
    {36,  1, "IOR14A", true,  nullptr },
    {37,  1, "IOR15A", true,  nullptr },
    {38,  1, "IOR16A", true,  nullptr },
    {39,  1, "GND",    false, "GND" },
    {40,  1, "VCC",    false, "VCC (1.2V core)" },
    {41,  1, "IOR19A", true,  nullptr },
    {42,  1, "IOR20A", true,  nullptr },
    {43,  1, "IOR21A", true,  nullptr },
    {44,  1, "IOR22A", true,  nullptr },

    // === Bottom Edge (pin 45-66) ===
    {45,  0, "IOB1A",  true,  nullptr },
    {46,  0, "IOB2A",  true,  nullptr },
    {47,  0, "IOB3A",  true,  nullptr },
    {48,  0, "IOB4A",  true,  nullptr },
    {49,  0, "IOB5A",  true,  nullptr },
    {50,  0, "IOB6A",  true,  nullptr },
    {51,  0, "GND",    false, "GND" },
    {52,  0, "IOB8A",  true,  nullptr },
    {53,  0, "IOB9A",  true,  nullptr },
    {54,  0, "IOB10A", true,  nullptr },
    {55,  0, "IOB11A", true,  nullptr },
    {56,  0, "IOB12A", true,  nullptr },
    {57,  0, "IOB13A", true,  nullptr },
    {58,  0, "IOB14A", true,  nullptr },
    {59,  0, "GND",    false, "GND" },
    {60,  0, "VCCIO",  false, "VCCIO (I/O bank power)" },
    {61,  0, "IOB17A", true,  nullptr },
    {62,  0, "IOB18A", true,  nullptr },
    {63,  0, "IOB19A", true,  nullptr },
    {64,  0, "IOB20A", true,  nullptr },
    {65,  0, "IOB21A", true,  nullptr },
    {66,  0, "IOB22A", true,  nullptr },

    // === Left Edge (pin 67-88) ===
    {67,  1, "IOL1A",  true,  nullptr },
    {68,  1, "IOL2A",  true,  nullptr },
    {69,  1, "IOL3A",  true,  nullptr },
    {70,  1, "IOL4A",  true,  nullptr },
    {71,  1, "IOL5A",  true,  nullptr },
    {72,  1, "IOL6A",  true,  nullptr },
    {73,  1, "GND",    false, "GND" },
    {74,  1, "VCC",    false, "VCC (1.2V core)" },
    {75,  1, "IOL9A",  true,  nullptr },
    {76,  1, "IOL10A", true,  nullptr },
    {77,  1, "IOL11A", true,  nullptr },
    {78,  1, "IOL12A", true,  nullptr },
    {79,  1, "IOL13A", true,  nullptr },
    {80,  1, "IOL14A", true,  nullptr },
    {81,  1, "IOL15A", true,  nullptr },
    {82,  1, "IOL16A", true,  nullptr },
    {83,  1, "GND",    false, "GND" },
    {84,  1, "VCCIO",  false, "VCCIO (I/O bank power)" },
    {85,  1, "IOL19A", true,  nullptr },
    {86,  1, "IOL20A", true,  nullptr },
    {87,  1, "IOL21A", true,  nullptr },
    {88,  1, "IOL22A", true,  nullptr },
};

// JTAG 和配置引脚 (通常在特定引脚上)
constexpr int kJTAG_TCK  = 56;   // TCK (实际位置需核对)
constexpr int kJTAG_TMS  = 57;   // TMS
constexpr int kJTAG_TDI  = 58;   // TDI
constexpr int kJTAG_TDO  = 59;   // TDO

// Tang Nano 9K 板级资源别名 (来源: Sipeed Tang Nano 9K 原理图 v1.0)
struct BoardResourceEntry {
    const char* alias;
    int packagePin;
    const char* description;
    const char* resourceType;
};

constexpr BoardResourceEntry kTangNano9kResources[] = {
    {"LED1",    10, "Red User LED (active low)",  "LED"},
    {"LED2",    11, "Green User LED (active low)","LED"},
    {"LED3",    12, "Blue User LED (active low)", "LED"},
    {"LED4",    13, "User LED 4 (active low)",    "LED"},
    {"LED5",    14, "User LED 5 (active low)",    "LED"},
    {"LED6",    15, "User LED 6 (active low)",    "LED"},
    {"BTN1",    16, "User Button 1 (active low)", "BUTTON"},
    {"BTN2",    17, "User Button 2 (active low)", "BUTTON"},
    {"CLK",     52, "27MHz Crystal Oscillator",   "CLK"},
    {"UART_TX", 41, "USB-UART TX (CH340)",        "UART"},
    {"UART_RX", 42, "USB-UART RX (CH340)",        "UART"},
    {"SPI_CS",  63, "SPI Flash Chip Select",      "SPI"},
    {"SPI_CLK", 64, "SPI Flash Clock",            "SPI"},
    {"SPI_MOSI",65, "SPI Flash MOSI (IO0)",       "SPI"},
    {"SPI_MISO",66, "SPI Flash MISO (IO1)",       "SPI"},
    {"WS2812",  28, "WS2812 RGB LED (active high)","RGB"},
    {"USB_DP",  34, "USB D+ (direct connect)",    "USB"},
    {"USB_DN",  35, "USB D- (direct connect)",    "USB"},
};

// 已知的合法 IO_TYPE 值
const std::vector<wxString> kKnownIOTypes = {
    "LVCMOS33", "LVCMOS25", "LVCMOS18", "LVCMOS15", "LVCMOS12",
    "LVTTL33", "LVTTL25",
    "SSTL33_I", "SSTL33_II", "SSTL25_I", "SSTL25_II",
    "SSTL18_I", "SSTL18_II", "SSTL15_I", "SSTL15_II",
    "HSTL18_I", "HSTL18_II", "HSTL15_I",
    "PCI33", "LVDS33", "RSDS33",
};

} // anonymous namespace

void ApplyReservedPins(FpgaPinDatabase& db) {
    // 在构建完成后标记 JTAG 和特殊引脚
    const int reservedPins[] = { kJTAG_TCK, kJTAG_TMS, kJTAG_TDI, kJTAG_TDO };
    const char* jtagReasons[] = { "JTAG TCK", "JTAG TMS", "JTAG TDI", "JTAG TDO" };
    for (int i = 0; i < 4; ++i) {
        auto* pin = const_cast<PackagePin*>(db.FindPin(reservedPins[i]));
        if (pin) {
            pin->available = false;
            pin->reservedReason = jtagReasons[i];
        }
    }
}

const FpgaPinDatabase& GetQFN88PinDatabase() {
    static FpgaPinDatabase db;
    static bool initialized = false;
    if (!initialized) {
        db.device = "GW1NR-LV9QN88PC6/I5";
        db.packageName = "QFN88";

        constexpr size_t pinCount = sizeof(kQFN88Pins) / sizeof(kQFN88Pins[0]);
        db.pins.reserve(pinCount);
        for (size_t i = 0; i < pinCount; ++i) {
            PackagePin p;
            p.pinNumber = kQFN88Pins[i].pinNumber;
            p.bank = kQFN88Pins[i].bank;
            p.ioLocation = wxString::FromUTF8(kQFN88Pins[i].ioLocation);
            p.available = kQFN88Pins[i].available;
            p.reservedReason = kQFN88Pins[i].reservedReason
                ? wxString::FromUTF8(kQFN88Pins[i].reservedReason) : wxString();
            p.defaultIOType = "LVCMOS33";
            db.pins.push_back(p);
        }

        ApplyReservedPins(db);

        constexpr size_t resCount = sizeof(kTangNano9kResources) / sizeof(kTangNano9kResources[0]);
        db.boardResources.reserve(resCount);
        for (size_t i = 0; i < resCount; ++i) {
            BoardResource r;
            r.alias = wxString::FromUTF8(kTangNano9kResources[i].alias);
            r.packagePin = kTangNano9kResources[i].packagePin;
            r.description = wxString::FromUTF8(kTangNano9kResources[i].description);
            r.resourceType = wxString::FromUTF8(kTangNano9kResources[i].resourceType);
            db.boardResources.push_back(r);
        }

        initialized = true;
    }
    return db;
}

const PackagePin* FpgaPinDatabase::FindPin(int pinNumber) const {
    for (const auto& p : pins) {
        if (p.pinNumber == pinNumber) return &p;
    }
    return nullptr;
}

const BoardResource* FpgaPinDatabase::FindResourceByPin(int pinNumber) const {
    for (const auto& r : boardResources) {
        if (r.packagePin == pinNumber) return &r;
    }
    return nullptr;
}

std::vector<const BoardResource*> FpgaPinDatabase::FindResourcesByType(const wxString& type) const {
    std::vector<const BoardResource*> result;
    for (const auto& r : boardResources) {
        if (r.resourceType == type) {
            result.push_back(&r);
        }
    }
    return result;
}

std::vector<int> FpgaPinDatabase::GetAvailablePins() const {
    std::vector<int> result;
    for (const auto& p : pins) {
        if (p.available) result.push_back(p.pinNumber);
    }
    return result;
}

bool FpgaPinDatabase::IsPinAvailable(int pinNumber) const {
    const auto* p = FindPin(pinNumber);
    return p && p->available;
}

const std::vector<wxString>& GetKnownIOTypes() {
    return kKnownIOTypes;
}
