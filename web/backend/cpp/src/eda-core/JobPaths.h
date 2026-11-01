#pragma once
#include <filesystem>
#include <eda/api/Types.h>
namespace eda::web {
std::filesystem::path CheckedPath(const std::filesystem::path& root,
                                  const std::filesystem::path& path, bool mustExist);
void AtomicJson(const std::filesystem::path& root, const std::filesystem::path& relative,
                const Json& value);
}  // namespace eda::web
