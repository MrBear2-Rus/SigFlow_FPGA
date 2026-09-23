#pragma once

#include <eda/api/target_profile.hpp>

#include <filesystem>
#include <string>

namespace eda {
namespace target {

// P1-5：解析 `target-profiles/*.json`（eda.target-profile.v1），取代硬编码。
class TargetProfileStore {
public:
    static bool Parse(const std::string& json, TargetProfile& profile, std::string& error);
    static bool LoadFile(const std::filesystem::path& file, TargetProfile& profile,
                         std::string& error);
    // 在目录下查找 <id>.json 并加载。
    static bool LoadById(const std::filesystem::path& directory, const std::string& id,
                         TargetProfile& profile, std::string& error);
};

} // namespace target
} // namespace eda
