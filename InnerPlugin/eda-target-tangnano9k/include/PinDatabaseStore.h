#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace eda {
namespace target {

struct PinEntry {
    int pin = 0;
    int bank = 0;
    std::string ioLocation;
    bool available = true;
    std::string reservedReason;
    std::string defaultIOType = "LVCMOS33";
};

struct BoardResourceEntry {
    std::string alias;
    int pin = 0;
    std::string description;
    std::string type;
};

// P1-5（收尾）：引脚库数据外提（取代 FpgaPinData 硬编码表）。
struct PinDatabase {
    std::string device;
    std::string packageName;
    std::vector<PinEntry> pins;
    std::vector<BoardResourceEntry> boardResources;
    std::vector<std::string> ioTypes;
};

class PinDatabaseStore {
public:
    static bool Parse(const std::string& json, PinDatabase& database, std::string& error);
    static bool LoadFile(const std::filesystem::path& file, PinDatabase& database,
                         std::string& error);
};

} // namespace target
} // namespace eda
