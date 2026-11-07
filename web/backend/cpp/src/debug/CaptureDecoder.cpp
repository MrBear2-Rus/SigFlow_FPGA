#include "CaptureDecoder.h"

#include "DebugProtocol.h"
#include "eda-core/JobPaths.h"

#include <eda/api/Types.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace eda::debug {
namespace {

constexpr std::uint32_t kMaxCaptureDepth = 1u << 20;
constexpr std::uint32_t kMaxCaptureCount = 1u << 20;
constexpr std::uint32_t kMaxProbeCount = 32;
constexpr std::size_t kHeaderSize = 32;

std::atomic<std::uint64_t> g_tempSequence{0};

std::string BitsOf(std::uint32_t value, unsigned width) {
    std::string bits(width, '0');
    for (unsigned bit = 0; bit < width; ++bit)
        bits[width - 1 - bit] = ((value >> bit) & 1u) != 0 ? '1' : '0';
    return bits;
}

std::string LeafName(const std::string& path) {
    const auto dot = path.rfind('.');
    return dot == std::string::npos ? path : path.substr(dot + 1);
}

bool IsVcdIdentifier(const std::string& value) {
    if (value.empty())
        return false;
    const auto isFirst = [](unsigned char c) { return std::isalpha(c) || c == '_'; };
    const auto isRest = [](unsigned char c) { return std::isalnum(c) || c == '_'; };
    if (!isFirst(static_cast<unsigned char>(value.front())))
        return false;
    return std::all_of(value.begin() + 1, value.end(),
                       [&](char c) { return isRest(static_cast<unsigned char>(c)); });
}

std::string IdCode(std::size_t index) {
    return std::string(1, static_cast<char>('!' + index));
}

class AtomicTextFile {
  public:
    explicit AtomicTextFile(const fs::path& path) {
        if (path.empty())
            throw std::invalid_argument("output path is empty");
        target_ = fs::absolute(path).lexically_normal();
        const auto parent = target_.parent_path();
        if (target_.filename().empty() || parent.empty())
            throw std::invalid_argument("output path must name a file");
        fs::create_directories(parent);
        target_ = web::CheckedPath(parent, target_.filename(), false);

        for (int attempt = 0; attempt < 16; ++attempt) {
            auto suffix = std::to_string(static_cast<unsigned long long>(getpid())) + "." +
                          std::to_string(g_tempSequence.fetch_add(1));
            temporary_ = web::CheckedPath(
                parent, "." + target_.filename().string() + ".tmp." + suffix, false);
            const int fd = open(temporary_.c_str(),
                                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (fd < 0) {
                if (errno == EEXIST)
                    continue;
                throw std::runtime_error("cannot create VCD temporary file: " +
                                         std::string(std::strerror(errno)));
            }
            file_ = fdopen(fd, "wb");
            if (file_ == nullptr) {
                const int savedErrno = errno;
                close(fd);
                std::error_code ignored;
                fs::remove(temporary_, ignored);
                throw std::runtime_error("cannot open VCD stream: " +
                                         std::string(std::strerror(savedErrno)));
            }
            return;
        }
        throw std::runtime_error("cannot allocate unique VCD temporary file");
    }

    AtomicTextFile(const AtomicTextFile&) = delete;
    AtomicTextFile& operator=(const AtomicTextFile&) = delete;

    ~AtomicTextFile() {
        if (file_ != nullptr)
            std::fclose(file_);
        if (!committed_ && !temporary_.empty()) {
            std::error_code ignored;
            fs::remove(temporary_, ignored);
        }
    }

    bool Write(std::string_view value) {
        return file_ != nullptr &&
               (value.empty() || std::fwrite(value.data(), 1, value.size(), file_) == value.size());
    }

    bool Commit(std::string& error) {
        if (file_ == nullptr) {
            error = "VCD output is not open";
            return false;
        }
        if (std::fflush(file_) != 0 || fsync(fileno(file_)) != 0) {
            error = "unable to flush VCD temporary file";
            return false;
        }
        if (std::fclose(file_) != 0) {
            file_ = nullptr;
            error = "unable to close VCD temporary file";
            return false;
        }
        file_ = nullptr;

        std::error_code ec;
        fs::rename(temporary_, target_, ec);
        if (ec) {
            error = "unable to commit VCD output: " + ec.message();
            return false;
        }
        committed_ = true;
        const int directory =
            open(target_.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0) {
            error = "unable to open VCD output directory for sync";
            return false;
        }
        const int syncStatus = fsync(directory);
        const int savedErrno = errno;
        close(directory);
        if (syncStatus != 0) {
            error =
                "unable to sync VCD output directory: " + std::string(std::strerror(savedErrno));
            return false;
        }
        return true;
    }

  private:
    fs::path target_;
    fs::path temporary_;
    std::FILE* file_ = nullptr;
    bool committed_ = false;
};

bool ReadExact(std::FILE* file, void* output, std::size_t size) {
    return std::fread(output, 1, size, file) == size;
}

bool ReadU32LE(std::FILE* file, std::uint32_t& value) {
    std::array<unsigned char, 4> bytes{};
    if (!ReadExact(file, bytes.data(), bytes.size()))
        return false;
    value = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
            (static_cast<std::uint32_t>(bytes[2]) << 16) |
            (static_cast<std::uint32_t>(bytes[3]) << 24);
    return true;
}

}  // namespace

bool DecodeCaptureToVcd(const std::vector<std::uint32_t>& samples, std::uint32_t depth,
                        std::uint16_t triggerIndex, const std::vector<CaptureProbe>& probes,
                        const std::string& path, CaptureVcdMeta& meta, std::string& error,
                        const CaptureTimebase& timebase) {
    error.clear();
    meta = CaptureVcdMeta{};
    if (depth == 0 || depth > kMaxCaptureDepth || samples.size() < depth || probes.empty() ||
        probes.size() > kMaxProbeCount) {
        error = "invalid capture input (depth, samples, or probe count)";
        return false;
    }
    if (timebase.decimation == 0 || timebase.decimation > 255) {
        error = "capture decimation must be in 1..255";
        return false;
    }
    if (timebase.triggerObserved && triggerIndex >= depth) {
        error = "trigger index out of range";
        return false;
    }
    for (const auto& probe : probes) {
        const auto leaf = LeafName(probe.path);
        if (probe.path.empty() || leaf.empty() || !IsVcdIdentifier(leaf) || probe.width == 0 ||
            probe.width > 32 || probe.bitOffset >= 32 || probe.width > 32 - probe.bitOffset) {
            error = "invalid capture probe name, width, or bit offset";
            return false;
        }
    }

    meta.sampleClockHz = timebase.sampleClockHz;
    meta.decimation = timebase.decimation;
    meta.triggerObserved = timebase.triggerObserved;
    std::uint64_t numerator = 0;
    std::uint64_t denominator = 1;
    if (timebase.sampleClockHz != 0) {
        numerator = 1000000000000ULL * timebase.decimation;
        denominator = timebase.sampleClockHz;
        if (denominator > numerator) {
            error = "sample interval is below 1ps resolution";
            return false;
        }
        const auto divisor = std::gcd(numerator, denominator);
        meta.periodNumeratorPs = numerator / divisor;
        meta.periodDenominator = denominator / divisor;
        meta.timeDomain = "physical";
        meta.timescale = "1ps";
    }

    const auto tickAt = [&](std::uint64_t index, std::uint64_t& tick) {
        if (timebase.sampleClockHz == 0) {
            tick = index;
            return true;
        }
        const __uint128_t product = static_cast<__uint128_t>(index) * numerator;
        const __uint128_t value = product / denominator;
        if (value > std::numeric_limits<std::uint64_t>::max())
            return false;
        tick = static_cast<std::uint64_t>(value);
        return true;
    };
    if (!tickAt(depth - 1, meta.lastSampleTime) ||
        (meta.triggerObserved && !tickAt(triggerIndex, meta.triggerTime))) {
        error = "capture time range exceeds uint64";
        return false;
    }

    try {
        AtomicTextFile out(path);
        std::string buffer;
        buffer.reserve(64 * 1024);
        const auto append = [&](std::string_view value) { buffer.append(value); };
        const auto flush = [&]() {
            if (buffer.empty())
                return true;
            const bool ok = out.Write(buffer);
            buffer.clear();
            return ok;
        };

        append("$date\n  SigFlow TraceBridge capture\n$end\n");
        append("$version\n  CaptureDecoder 2.0\n$end\n");
        append("$timescale " + meta.timescale + " $end\n");
        append("$comment sigflow_time_domain " + meta.timeDomain + " $end\n");
        append("$comment sample_clock_hz " + std::to_string(meta.sampleClockHz) + " decimation " +
               std::to_string(meta.decimation) + " $end\n");
        append("$comment capture_layout linear_address_order $end\n");
        append("$comment trigger_index " + std::to_string(triggerIndex) + " $end\n");
        append("$comment trigger_observed " + std::to_string(meta.triggerObserved ? 1 : 0) +
               " $end\n");
        if (meta.triggerObserved)
            append("$comment trigger_time " + std::to_string(meta.triggerTime) + " $end\n");
        append("$scope module debug $end\n");
        for (std::size_t i = 0; i < probes.size(); ++i) {
            append("$var wire " + std::to_string(probes[i].width) + " " + IdCode(i) + " " +
                   LeafName(probes[i].path) + " $end\n");
        }
        append("$upscope $end\n$enddefinitions $end\n");

        std::vector<std::string> previous(probes.size());
        for (std::uint32_t index = 0; index < depth; ++index) {
            std::uint64_t tick = 0;
            if (!tickAt(index, tick)) {
                error = "capture time range exceeds uint64";
                return false;
            }
            append("#" + std::to_string(tick) + "\n");
            const auto sample = samples[index];
            for (std::size_t i = 0; i < probes.size(); ++i) {
                const auto width = probes[i].width;
                const std::uint32_t mask = width == 32 ? 0xFFFFFFFFu : (1u << width) - 1u;
                const auto value = BitsOf((sample >> probes[i].bitOffset) & mask, width);
                if (index == 0 || value != previous[i]) {
                    append("b" + value + " " + IdCode(i) + "\n");
                    previous[i] = value;
                }
            }
            if (buffer.size() >= 64 * 1024 && !flush()) {
                error = "unable to write VCD temporary file";
                return false;
            }
        }
        if (!flush()) {
            error = "unable to write VCD temporary file";
            return false;
        }
        if (!out.Commit(error))
            return false;
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool SaveCaptureMetadata(const std::string& path, const CaptureVcdMeta& meta, std::uint32_t depth,
                         std::uint16_t triggerIndex, std::string& error) {
    error.clear();
    if (depth == 0 || depth > kMaxCaptureDepth || meta.decimation == 0 ||
        (meta.triggerObserved && triggerIndex >= depth) ||
        (meta.timeDomain != "physical" && meta.timeDomain != "sample_index")) {
        error = "invalid capture metadata";
        return false;
    }
    try {
        Json root = Json::object();
        root["schema_version"] = "1.0";
        root["time_domain"] = meta.timeDomain;
        root["vcd_timescale"] = meta.timescale;
        root["sample_clock_hz"] = meta.sampleClockHz;
        root["clock_source"] = meta.sampleClockHz == 0 ? "unknown" : "contract";
        root["decimation"] = meta.decimation;
        root["period_numerator_ps"] = meta.periodNumeratorPs;
        root["period_denominator"] = meta.periodDenominator;
        root["quantization"] =
            meta.timeDomain == "physical" ? "floor_absolute_1ps" : "sample_index";
        root["sample_enable_assumption"] = "continuous";
        root["capture_layout"] = "linear_address_order";
        root["sample_count"] = depth;
        root["trigger_index"] = triggerIndex;
        root["trigger_observed"] = meta.triggerObserved;
        root["trigger_time"] = meta.triggerObserved ? Json(meta.triggerTime) : Json(nullptr);
        root["last_sample_time"] = meta.lastSampleTime;

        const fs::path target = fs::absolute(path).lexically_normal();
        if (target.filename().empty())
            throw std::invalid_argument("metadata path must name a file");
        web::AtomicJson(target.parent_path(), target.filename(), root);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool LoadCaptureRaw(const std::string& path, std::uint32_t& depth, std::uint32_t& width,
                    std::uint16_t& start, std::vector<std::uint32_t>& samples, std::string& error) {
    error.clear();
    depth = 0;
    width = 0;
    start = 0;
    samples.clear();
    std::FILE* file = nullptr;
    try {
        if (path.empty())
            throw std::invalid_argument("capture.raw path is empty");
        const fs::path input = fs::absolute(path).lexically_normal();
        const auto checked = web::CheckedPath(input.parent_path(), input.filename(), true);
        const int fd = open(checked.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0)
            throw std::runtime_error("cannot open capture.raw: " +
                                     std::string(std::strerror(errno)));
        struct stat status{};
        if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) {
            close(fd);
            throw std::runtime_error("capture.raw is not a regular file");
        }
        file = fdopen(fd, "rb");
        if (file == nullptr) {
            const int savedErrno = errno;
            close(fd);
            throw std::runtime_error("cannot open capture.raw stream: " +
                                     std::string(std::strerror(savedErrno)));
        }
        if (status.st_size < static_cast<off_t>(kHeaderSize))
            throw std::runtime_error("truncated capture.raw header");
        std::array<char, 8> magic{};
        if (!ReadExact(file, magic.data(), magic.size()) ||
            std::string_view(magic.data(), magic.size()) != "SFDBGRAW")
            throw std::runtime_error("bad capture.raw magic");

        std::uint32_t version = 0;
        std::uint32_t fileDepth = 0;
        std::uint32_t fileWidth = 0;
        std::uint32_t fileStart = 0;
        std::uint32_t count = 0;
        std::uint32_t reserved = 0;
        if (!ReadU32LE(file, version) || !ReadU32LE(file, fileDepth) ||
            !ReadU32LE(file, fileWidth) || !ReadU32LE(file, fileStart) || !ReadU32LE(file, count) ||
            !ReadU32LE(file, reserved))
            throw std::runtime_error("truncated capture.raw header");
        if (version != 1 || reserved != 0 || fileDepth == 0 || fileDepth > kMaxCaptureDepth ||
            fileWidth == 0 || fileWidth > 32 ||
            fileStart > std::numeric_limits<std::uint16_t>::max() || count == 0 ||
            count > kMaxCaptureCount || static_cast<std::uint64_t>(fileStart) + count > fileDepth) {
            throw std::runtime_error("unsupported or invalid capture.raw header");
        }
        const auto expectedSize = static_cast<std::uint64_t>(kHeaderSize) +
                                  static_cast<std::uint64_t>(count) * sizeof(std::uint32_t);
        if (static_cast<std::uint64_t>(status.st_size) != expectedSize)
            throw std::runtime_error("capture.raw payload size mismatch");

        std::vector<std::uint32_t> decoded;
        decoded.reserve(count);
        const std::uint32_t mask = fileWidth == 32 ? 0xFFFFFFFFu : (1u << fileWidth) - 1u;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t sample = 0;
            if (!ReadU32LE(file, sample))
                throw std::runtime_error("truncated capture.raw payload");
            if ((sample & ~mask) != 0)
                throw std::runtime_error("capture.raw sample exceeds declared width");
            decoded.push_back(sample);
        }
        if (std::fgetc(file) != EOF)
            throw std::runtime_error("capture.raw contains trailing data");
        if (std::fclose(file) != 0) {
            file = nullptr;
            throw std::runtime_error("unable to close capture.raw");
        }
        file = nullptr;
        depth = fileDepth;
        width = fileWidth;
        start = static_cast<std::uint16_t>(fileStart);
        samples = std::move(decoded);
        return true;
    } catch (const std::exception& exception) {
        if (file != nullptr)
            std::fclose(file);
        error = exception.what();
        return false;
    }
}

}  // namespace eda::debug
