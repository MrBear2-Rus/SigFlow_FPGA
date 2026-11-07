#include "debug/CaptureDecoder.h"
#include "debug/DebugProtocol.h"

#include <eda/api/Types.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct TempDirectory {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("sigflow-capture-" + std::to_string(getpid()));
    TempDirectory() { std::filesystem::create_directories(path); }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

}  // namespace

int main() {
    try {
        TempDirectory temporary;
        const auto vcdPath = temporary.path / "capture.vcd";
        const auto metadataPath = temporary.path / "capture.meta.json";
        const std::vector<std::uint32_t> samples{0b0101, 0b0110, 0b0001};
        const std::vector<eda::debug::CaptureProbe> probes{{"state", "top.state", 2, 1},
                                                           {"flag", "top.flag", 1, 0}};
        eda::debug::CaptureVcdMeta meta;
        std::string error;
        const eda::debug::CaptureTimebase timebase{50000000, 1, true};
        Check(eda::debug::DecodeCaptureToVcd(samples, 3, 1, probes, vcdPath.string(), meta, error,
                                             timebase),
              "decode VCD");
        Check(meta.timeDomain == "physical" && meta.timescale == "1ps" &&
                  meta.triggerTime == 20000 && meta.lastSampleTime == 40000,
              "absolute rational time conversion");
        const auto vcd = ReadText(vcdPath);
        Check(vcd.find("$var wire 2 ! state $end") != std::string::npos &&
                  vcd.find("#0\nb10 !\nb1 \"\n#20000\nb11 !\nb0 \"\n#40000\nb00 !\nb1 \"\n") !=
                      std::string::npos,
              "linear values and transition-only output");
        Check(eda::debug::SaveCaptureMetadata(metadataPath.string(), meta, 3, 1, error),
              "save metadata");
        std::ifstream metadataFile(metadataPath);
        eda::Json metadata;
        metadataFile >> metadata;
        Check(metadata.at("schema_version") == "1.0" &&
                  metadata.at("trigger_time").get<std::uint64_t>() == 20000 &&
                  metadata.at("last_sample_time").get<std::uint64_t>() == 40000,
              "metadata values");

        const auto preserved = temporary.path / "preserved.vcd";
        {
            std::ofstream output(preserved);
            output << "keep";
        }
        auto invalidProbes = probes;
        invalidProbes.front().path = "top.bad\n$end";
        Check(!eda::debug::DecodeCaptureToVcd(samples, 3, 1, invalidProbes, preserved.string(),
                                              meta, error, timebase),
              "reject VCD injection in name");
        Check(ReadText(preserved) == "keep", "validation failure preserves output");
        Check(!eda::debug::DecodeCaptureToVcd(samples, 3, 0, probes, vcdPath.string(), meta, error,
                                              {std::numeric_limits<std::uint64_t>::max(), 1, true}),
              "reject sub-picosecond timebase");

        const auto rawPath = temporary.path / "capture.raw";
        Check(eda::debug::SaveCaptureRaw(rawPath.string(), 3, 32, 0, samples, error), "save raw");
        std::uint32_t depth = 0;
        std::uint32_t width = 0;
        std::uint16_t start = 99;
        std::vector<std::uint32_t> loaded;
        Check(eda::debug::LoadCaptureRaw(rawPath.string(), depth, width, start, loaded, error),
              "load raw");
        Check(depth == 3 && width == 32 && start == 0 && loaded == samples, "raw roundtrip fields");
        Check(!eda::debug::SaveCaptureRaw(rawPath.string(), 2, 32, 0, samples, error),
              "reject inconsistent raw range");
        Check(eda::debug::LoadCaptureRaw(rawPath.string(), depth, width, start, loaded, error) &&
                  loaded == samples,
              "invalid raw save preserves existing artifact");
        const auto invalidRawPath = temporary.path / "invalid.raw";
        Check(!eda::debug::SaveCaptureRaw(invalidRawPath.string(), 3, 1, 0, samples, error),
              "reject samples beyond declared raw width");
        Check(!std::filesystem::exists(invalidRawPath), "invalid raw save creates no artifact");
        {
            std::ofstream append(rawPath, std::ios::binary | std::ios::app);
            append.put('x');
        }
        Check(!eda::debug::LoadCaptureRaw(rawPath.string(), depth, width, start, loaded, error),
              "reject raw trailing data");
        Check(depth == 0 && width == 0 && start == 0 && loaded.empty(),
              "raw failure clears outputs");
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
