#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace eda::debug {

struct CaptureProbe {
    std::string id;
    std::string path;
    unsigned width = 1;
    unsigned bitOffset = 0;
};

struct CaptureTimebase {
    std::uint64_t sampleClockHz = 0;
    std::uint32_t decimation = 1;
    bool triggerObserved = true;
};

// All times are VCD ticks. Physical captures use 1ps and absolute rational
// conversion floor(i * decimation * 10^12 / sampleClockHz), without step drift.
struct CaptureVcdMeta {
    std::uint64_t triggerTime = 0;
    std::string timescale = "1ns";
    std::string timeDomain = "sample_index";
    std::uint64_t sampleClockHz = 0;
    std::uint32_t decimation = 1;
    std::uint64_t periodNumeratorPs = 0;
    std::uint64_t periodDenominator = 0;
    std::uint64_t lastSampleTime = 0;
    bool triggerObserved = false;
};

// Samples remain in linear address order. triggerIndex identifies a sample in
// that sequence and does not imply circular-buffer rotation.
bool DecodeCaptureToVcd(const std::vector<std::uint32_t>& samples, std::uint32_t depth,
                        std::uint16_t triggerIndex, const std::vector<CaptureProbe>& probes,
                        const std::string& path, CaptureVcdMeta& meta, std::string& error,
                        const CaptureTimebase& timebase = {});

bool SaveCaptureMetadata(const std::string& path, const CaptureVcdMeta& meta, std::uint32_t depth,
                         std::uint16_t triggerIndex, std::string& error);

// Reads the SFDBGRAW v1 file written by SaveCaptureRaw in DebugProtocol.
bool LoadCaptureRaw(const std::string& path, std::uint32_t& depth, std::uint32_t& width,
                    std::uint16_t& start, std::vector<std::uint32_t>& samples, std::string& error);

}  // namespace eda::debug
