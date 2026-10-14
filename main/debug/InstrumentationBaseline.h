#pragma once

#include "InstrumentationImpact.h"

#include <string>
#include <vector>

namespace sigflow::debug {

struct InstrumentationBaselineRecord {
    std::string baselineId;
    std::string reportPath;
    std::string sourceFingerprint;
    std::string createdUtc;
    InstrumentationBuildIdentity identity;
};

struct InstrumentationBaselineSelection {
    std::string baselineId;
    std::string reportPath;
    bool valid = false;
    std::string invalidReason;
};

class InstrumentationBaselineCatalog {
public:
    std::string schemaVersion = "1.0";

    bool Add(const InstrumentationBaselineRecord& record, std::string& error);
    bool Select(const std::string& baselineId,
                const std::string& currentSourceFingerprint,
                InstrumentationBaselineSelection& out,
                std::string& error) const;

    bool Save(const std::string& path, std::string& error) const;
    bool Load(const std::string& path, std::string& error);

    const std::vector<InstrumentationBaselineRecord>& Records() const { return records_; }

private:
    std::vector<InstrumentationBaselineRecord> records_;
};

} // namespace sigflow::debug
