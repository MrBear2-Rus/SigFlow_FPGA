#pragma once

#include "WaveProtocolDecoder.h"

#include <string>
#include <vector>

namespace sigflow::wave {

// Stable, persistable wrapper around decoder output.  The wrapper carries the
// source identity and time semantics that a bare ProtocolTransaction cannot
// provide when it is reopened by TB-13/TB-23.
struct ProtocolEventReport {
    std::string schemaVersion = "1.0";
    std::string decoderVersion = "1.0";
    std::string captureId;
    std::string sourcePath;
    std::string timeDomain = "unknown";
    std::string timescale;
    std::string evidencePath;
    std::string configurationHash;
    std::vector<ProtocolTransaction> transactions;
    std::vector<std::string> diagnostics;
};

bool BuildProtocolEventReport(const sigflow::trace::TraceSource& source,
                              const std::string& captureId,
                              const std::string& evidencePath,
                              const std::string& configurationHash,
                              const std::vector<ProtocolTransaction>& transactions,
                              ProtocolEventReport& out,
                              std::string& error);

std::string GenerateProtocolEventReportJson(const ProtocolEventReport& report);

std::string GenerateProtocolEventReportCsv(const ProtocolEventReport& report);

bool SaveProtocolEventReport(const ProtocolEventReport& report,
                             const std::string& path,
                             std::string& error);

bool SaveProtocolEventReportCsv(const ProtocolEventReport& report,
                                const std::string& path,
                                std::string& error);

bool LoadProtocolEventReport(const std::string& path,
                             ProtocolEventReport& out,
                             std::string& error);

} // namespace sigflow::wave
