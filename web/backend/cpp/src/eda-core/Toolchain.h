#pragma once

#include <eda/api/toolchain.hpp>

#include <functional>
#include <string>
#include <vector>

namespace eda {

class DefaultToolchain final : public IToolchain {
public:
    using EnvironmentLookup = std::function<std::string(const std::string&)>;

    explicit DefaultToolchain(std::vector<std::filesystem::path> bundledRoots = {},
                              EnvironmentLookup environmentLookup = {});

    ToolResolution Resolve(const ToolQuery& query) const override;
    std::string ExecutableSuffix() const override;
    char PathListSeparator() const override;

private:
    std::vector<std::filesystem::path> bundledRoots_;
    EnvironmentLookup environmentLookup_;
};

} // namespace eda
