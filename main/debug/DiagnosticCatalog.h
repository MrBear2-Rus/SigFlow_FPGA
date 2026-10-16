#pragma once

#include <string>
#include <vector>

#include "CaptureRunService.h"

namespace sigflow {
namespace debug {

// Stable, host-side description of a diagnostic code.  Producers may still
// emit codes that are not in this first-edition catalog; lookup is advisory so
// older plugins and board firmware remain forward compatible.
struct DiagnosticCodeDescriptor {
    std::string code;
    std::string stage;
    DiagnosticSeverity defaultSeverity = DiagnosticSeverity::Error;
    std::string summary;
    std::string action;
};

// The returned collection is immutable and process-lifetime stable.
const std::vector<DiagnosticCodeDescriptor>& KnownDiagnosticCodes();

// Returns nullptr for an unknown code.  Unknown codes are intentionally not
// rejected: the registry is a shared vocabulary, not a schema gate.
const DiagnosticCodeDescriptor* FindDiagnosticCode(const std::string& code);

inline bool IsKnownDiagnosticCode(const std::string& code)
{
    return FindDiagnosticCode(code) != nullptr;
}

} // namespace debug
} // namespace sigflow
