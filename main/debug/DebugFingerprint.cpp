#include "DebugFingerprint.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace sigflow {
namespace debug {

namespace {

constexpr std::size_t kHashBufferSize = 64 * 1024;

bool HashBytes(const unsigned char* data, std::size_t size, unsigned char* digest,
               std::size_t digestSize)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD bytesReturned = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        return false;
    }
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
                          &bytesReturned, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength),
                          &bytesReturned, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    std::vector<unsigned char> object(objectLength);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    bool ok = BCryptHashData(hash, const_cast<PUCHAR>(data),
                             static_cast<ULONG>(size), 0) >= 0 &&
              BCryptFinishHash(hash, digest, static_cast<ULONG>(digestSize), 0) >= 0;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

std::string HexDigest(const unsigned char* digest, std::size_t size)
{
    static const char* kHex = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        result.push_back(kHex[digest[i] >> 4]);
        result.push_back(kHex[digest[i] & 0x0F]);
    }
    return result;
}

} // namespace

std::string Sha256String(const std::string& data)
{
    unsigned char digest[32] = { 0 };
    if (!HashBytes(reinterpret_cast<const unsigned char*>(data.data()), data.size(),
                   digest, sizeof(digest))) {
        return std::string();
    }
    return HexDigest(digest, sizeof(digest));
}

std::string Sha256File(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::string();

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD bytesReturned = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        return std::string();
    }
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
                          &bytesReturned, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength),
                          &bytesReturned, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return std::string();
    }
    std::vector<unsigned char> object(objectLength);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return std::string();
    }

    std::vector<char> buffer(kHashBufferSize);
    bool ok = true;
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = in.gcount();
        if (count > 0 &&
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                           static_cast<ULONG>(count), 0) < 0) {
            ok = false;
            break;
        }
        if (count < static_cast<std::streamsize>(buffer.size())) break;
    }
    unsigned char digest[32] = { 0 };
    if (ok) {
        ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return std::string();
    return HexDigest(digest, sizeof(digest));
}

std::string ComputeSourceFingerprint(const DebugFingerprintInputs& inputs)
{
    std::ostringstream stream;
    for (const std::string& file : inputs.sourceFiles) {
        const std::string hash = Sha256File(file);
        stream << file << ":" << hash << "\n";
    }
    stream << "top=" << inputs.topModule << "\n";
    stream << "constraints=" << Sha256String(inputs.constraintsContent) << "\n";
    stream << "target=" << inputs.targetProfile << "\n";
    stream << "probes=" << Sha256String(inputs.probeSignature) << "\n";
    return Sha256String(stream.str());
}

std::string ComputeToolchainFingerprint(const std::vector<std::string>& toolVersions,
                                        const std::string& debugIpVersion)
{
    std::ostringstream stream;
    for (const std::string& version : toolVersions) {
        stream << version << "\n";
    }
    stream << "debug_ip=" << debugIpVersion << "\n";
    return Sha256String(stream.str());
}

std::uint64_t Fingerprint64FromHex(const std::string& sha256Hex)
{
    std::uint64_t value = 0;
    // 取最后 16 个 hex 字符
    const std::size_t start = sha256Hex.size() >= 16 ? sha256Hex.size() - 16 : 0;
    for (std::size_t i = start; i < sha256Hex.size(); ++i) {
        const char c = sha256Hex[i];
        unsigned nibble = 0;
        if (c >= '0' && c <= '9') nibble = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') nibble = static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') nibble = static_cast<unsigned>(c - 'A' + 10);
        else continue;
        value = (value << 4) | nibble;
    }
    return value;
}

} // namespace debug
} // namespace sigflow
