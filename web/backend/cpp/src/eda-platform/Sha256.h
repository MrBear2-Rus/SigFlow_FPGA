#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace eda {
namespace platform {

// 可移植 SHA-256（不依赖 wx / 平台 crypto）。
std::string Sha256Hex(const void* data, std::size_t size);
std::string Sha256FileHex(const std::filesystem::path& path);

} // namespace platform
} // namespace eda
