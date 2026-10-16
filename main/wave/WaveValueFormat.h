#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace sigflow::wave {

// Display formatting is a presentation choice.  The trace source remains the
// canonical binary/real/string value and is never rewritten by this layer.
enum class WaveDisplayFormat {
    Binary,
    Hex,
    Unsigned,
    Signed,
};

inline const char* WaveDisplayFormatName(WaveDisplayFormat format)
{
    switch (format) {
        case WaveDisplayFormat::Binary: return "binary";
        case WaveDisplayFormat::Hex: return "hex";
        case WaveDisplayFormat::Unsigned: return "unsigned";
        case WaveDisplayFormat::Signed: return "signed";
    }
    return "binary";
}

inline bool WaveDisplayFormatFromName(const std::string& name,
                                      WaveDisplayFormat& format)
{
    if (name == "binary") format = WaveDisplayFormat::Binary;
    else if (name == "hex") format = WaveDisplayFormat::Hex;
    else if (name == "unsigned") format = WaveDisplayFormat::Unsigned;
    else if (name == "signed") format = WaveDisplayFormat::Signed;
    else return false;
    return true;
}

inline std::string WaveTrimValue(const std::string& value)
{
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

inline bool WaveIsBinary(const std::string& value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(),
        [](char c) { return c == '0' || c == '1'; });
}

inline std::string WaveBinaryToDecimal(const std::string& bits)
{
    std::vector<unsigned char> digits(1, 0); // little-endian decimal digits
    for (char bit : bits) {
        unsigned carry = static_cast<unsigned>(bit - '0');
        for (auto& digit : digits) {
            const unsigned value = static_cast<unsigned>(digit) * 2u + carry;
            digit = static_cast<unsigned char>(value % 10u);
            carry = value / 10u;
        }
        while (carry != 0) {
            digits.push_back(static_cast<unsigned char>(carry % 10u));
            carry /= 10u;
        }
    }
    while (digits.size() > 1 && digits.back() == 0) digits.pop_back();
    std::string result;
    result.reserve(digits.size());
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        result.push_back(static_cast<char>('0' + *it));
    }
    return result;
}

inline std::string WaveTwosComplementMagnitude(std::string bits)
{
    for (char& bit : bits) bit = bit == '1' ? '0' : '1';
    int carry = 1;
    for (auto it = bits.rbegin(); it != bits.rend() && carry != 0; ++it) {
        if (*it == '0') {
            *it = '1';
            carry = 0;
        } else {
            *it = '0';
        }
    }
    return WaveBinaryToDecimal(bits);
}

inline std::string WaveBinaryToHex(const std::string& bits, unsigned width)
{
    const unsigned shownWidth = width == 0 ? static_cast<unsigned>(bits.size()) : width;
    const std::size_t digits = std::max<std::size_t>(1, (shownWidth + 3u) / 4u);
    std::string out;
    out.reserve(digits + 2);
    out = "0x";
    const std::size_t padding = digits * 4 > bits.size() ? digits * 4 - bits.size() : 0;
    std::string padded(padding, '0');
    padded += bits;
    static constexpr char hex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < padded.size(); i += 4) {
        unsigned nibble = 0;
        for (std::size_t j = 0; j < 4; ++j) nibble = (nibble << 1) | (padded[i + j] - '0');
        out.push_back(hex[nibble]);
    }
    return out;
}

inline std::string FormatWaveValue(const std::string& raw, WaveDisplayFormat format,
                                   unsigned width = 0)
{
    const std::string value = WaveTrimValue(raw);
    if (value.empty() || format == WaveDisplayFormat::Binary || !WaveIsBinary(value)) return value;
    if (format == WaveDisplayFormat::Hex) return WaveBinaryToHex(value, width);
    if (format == WaveDisplayFormat::Unsigned) return WaveBinaryToDecimal(value);
    if (value.front() == '0') return WaveBinaryToDecimal(value);
    return "-" + WaveTwosComplementMagnitude(value);
}

} // namespace sigflow::wave
