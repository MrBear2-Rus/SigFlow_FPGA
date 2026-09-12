#pragma once

#include <wx/string.h>

#include <cstddef>
#include <cstdint>

// 可移植 SHA-256：Job 层的哈希记账不再依赖 Windows BCrypt，便于 B 流 Linux 移植。
wxString Sha256Hex(const void* data, std::size_t size);
wxString Sha256FileHex(const wxString& path);
