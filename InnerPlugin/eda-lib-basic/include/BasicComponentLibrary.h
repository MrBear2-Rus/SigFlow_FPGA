#pragma once

#include <eda/api/component_library.hpp>

#include <filesystem>
#include <string>

namespace eda {
namespace lib {

// P2-5：从 canvas_elements.json 解析元件库（wx-free）。
class BasicComponentLibrary final : public IComponentLibrary {
public:
    static bool Parse(const std::string& json, BasicComponentLibrary& library, std::string& error);
    static bool LoadFile(const std::filesystem::path& file, BasicComponentLibrary& library,
                         std::string& error);

    const std::vector<ComponentTemplate>& Components() const override { return components_; }
    const ComponentTemplate* Find(const std::string& type) const override;

private:
    std::vector<ComponentTemplate> components_;
};

} // namespace lib
} // namespace eda
