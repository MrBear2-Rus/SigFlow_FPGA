#include "Sha256.h"

#include "eda-platform/Sha256.h"

#include "platform/PlatformPaths.h"

// P1-1：SHA-256 算法已移入 core/src/eda-platform（std 版）；此处保留原 wx API 作为转发。
wxString Sha256Hex(const void* data, std::size_t size) {
    return wxString::FromUTF8(eda::platform::Sha256Hex(data, size).c_str());
}

wxString Sha256FileHex(const wxString& path) {
    return wxString::FromUTF8(
        eda::platform::Sha256FileHex(sigflow::platform::Utf8Path(path)).c_str());
}
