#include "Sha256.h"

#include <wx/file.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace {

constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

inline std::uint32_t RotateRight(std::uint32_t value, unsigned bits)
{
    return (value >> bits) | (value << (32u - bits));
}

class Sha256 {
public:
    Sha256()
        : state_{ 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                  0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u }
    {
    }

    void Update(const unsigned char* data, std::size_t size)
    {
        totalBytes_ += size;
        while (size > 0) {
            const std::size_t take = std::min(size, sizeof(buffer_) - buffered_);
            std::memcpy(buffer_ + buffered_, data, take);
            buffered_ += take;
            data += take;
            size -= take;
            if (buffered_ == sizeof(buffer_)) {
                Transform(buffer_);
                buffered_ = 0;
            }
        }
    }

    std::array<unsigned char, 32> Finish()
    {
        const std::uint64_t bitLength = totalBytes_ * 8u;
        const unsigned char padding = 0x80;
        Update(&padding, 1);
        const unsigned char zero = 0x00;
        while (buffered_ != 56) Update(&zero, 1);
        unsigned char length[8];
        for (int index = 0; index < 8; ++index) {
            length[index] = static_cast<unsigned char>(bitLength >> (56 - 8 * index));
        }
        Update(length, sizeof(length));
        std::array<unsigned char, 32> digest{};
        for (std::size_t index = 0; index < state_.size(); ++index) {
            digest[index * 4 + 0] = static_cast<unsigned char>(state_[index] >> 24);
            digest[index * 4 + 1] = static_cast<unsigned char>(state_[index] >> 16);
            digest[index * 4 + 2] = static_cast<unsigned char>(state_[index] >> 8);
            digest[index * 4 + 3] = static_cast<unsigned char>(state_[index]);
        }
        return digest;
    }

private:
    void Transform(const unsigned char block[64])
    {
        std::uint32_t schedule[64];
        for (int index = 0; index < 16; ++index) {
            schedule[index] = (static_cast<std::uint32_t>(block[index * 4 + 0]) << 24) |
                              (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
                              (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
                              (static_cast<std::uint32_t>(block[index * 4 + 3]));
        }
        for (int index = 16; index < 64; ++index) {
            const std::uint32_t s0 = RotateRight(schedule[index - 15], 7) ^
                                     RotateRight(schedule[index - 15], 18) ^
                                     (schedule[index - 15] >> 3);
            const std::uint32_t s1 = RotateRight(schedule[index - 2], 17) ^
                                     RotateRight(schedule[index - 2], 19) ^
                                     (schedule[index - 2] >> 10);
            schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
        }
        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (int index = 0; index < 64; ++index) {
            const std::uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
            const std::uint32_t choice = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + s1 + choice + kRoundConstants[index] + schedule[index];
            const std::uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{};
    unsigned char buffer_[64]{};
    std::size_t buffered_ = 0;
    std::uint64_t totalBytes_ = 0;
};

} // namespace

wxString Sha256Hex(const void* data, std::size_t size)
{
    Sha256 hash;
    if (data != nullptr && size > 0) {
        hash.Update(static_cast<const unsigned char*>(data), size);
    }
    const std::array<unsigned char, 32> digest = hash.Finish();
    wxString result;
    result.reserve(64);
    for (unsigned char byte : digest) result += wxString::Format("%02x", byte);
    return result;
}

wxString Sha256FileHex(const wxString& path)
{
    wxFile file(path, wxFile::read);
    if (!file.IsOpened()) return wxString();
    Sha256 hash;
    std::vector<unsigned char> buffer(64 * 1024);
    while (true) {
        const wxFileOffset read = file.Read(buffer.data(), buffer.size());
        if (read == wxInvalidOffset) return wxString();
        if (read == 0) break;
        hash.Update(buffer.data(), static_cast<std::size_t>(read));
    }
    const std::array<unsigned char, 32> digest = hash.Finish();
    wxString result;
    result.reserve(64);
    for (unsigned char byte : digest) result += wxString::Format("%02x", byte);
    return result;
}
