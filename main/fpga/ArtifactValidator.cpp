#include "ArtifactValidator.h"

#include "YosysArtifactValidator.h"

#include "platform/PlatformPaths.h"

// P1-1：产物校验逻辑已移入 InnerPlugin/eda-synth-yosys（wx-free）。
// 本文件保留同签名的 wx 适配层，供旧调用点（MainFrame / fpga_flow_probe）在迁移期使用。
wxString ToString(ArtifactValidationStatus status) {
    return wxString::FromUTF8(
        eda::synth::ToString(static_cast<eda::synth::ArtifactValidationStatus>(status)).c_str());
}

ArtifactValidator::ArtifactValidator(ArtifactValidationOptions options) : m_options(options) {}

bool ArtifactValidator::ValidateYosysJson(const wxString& jsonPath,
                                          const wxString& expectedTopModule,
                                          NetlistArtifactReport& report) const {
    eda::synth::ArtifactValidationOptions options;
    options.minimumFileSizeBytes = m_options.minimumFileSizeBytes;
    eda::synth::ArtifactValidator validator(options);

    eda::synth::NetlistArtifactReport pluginReport;
    const bool ok = validator.ValidateYosysJson(sigflow::platform::Utf8String(jsonPath),
                                                sigflow::platform::Utf8String(expectedTopModule),
                                                pluginReport);

    report = NetlistArtifactReport();
    report.status = static_cast<ArtifactValidationStatus>(pluginReport.status);
    report.artifactId = wxString::FromUTF8(pluginReport.artifactId.c_str());
    report.path = wxString::FromUTF8(pluginReport.path.c_str());
    report.expectedTopModule = wxString::FromUTF8(pluginReport.expectedTopModule.c_str());
    report.failedField = wxString::FromUTF8(pluginReport.failedField.c_str());
    report.message = wxString::FromUTF8(pluginReport.message.c_str());
    report.validatedAt = wxString::FromUTF8(pluginReport.validatedAt.c_str());
    report.sizeBytes = pluginReport.sizeBytes;
    report.portCount = pluginReport.portCount;
    report.cellCount = pluginReport.cellCount;
    report.netnameCount = pluginReport.netnameCount;
    report.sha256 = wxString::FromUTF8(pluginReport.sha256.c_str());
    return ok;
}

bool ArtifactValidator::WriteManifest(const wxString& manifestPath,
                                      const NetlistArtifactReport& report,
                                      wxString& errorMessage) const {
    eda::synth::ArtifactValidationOptions options;
    options.minimumFileSizeBytes = m_options.minimumFileSizeBytes;
    eda::synth::ArtifactValidator validator(options);

    eda::synth::NetlistArtifactReport pluginReport;
    pluginReport.status = static_cast<eda::synth::ArtifactValidationStatus>(report.status);
    pluginReport.artifactId = sigflow::platform::Utf8String(report.artifactId);
    pluginReport.path = sigflow::platform::Utf8String(report.path);
    pluginReport.expectedTopModule = sigflow::platform::Utf8String(report.expectedTopModule);
    pluginReport.failedField = sigflow::platform::Utf8String(report.failedField);
    pluginReport.message = sigflow::platform::Utf8String(report.message);
    pluginReport.validatedAt = sigflow::platform::Utf8String(report.validatedAt);
    pluginReport.sizeBytes = report.sizeBytes;
    pluginReport.portCount = report.portCount;
    pluginReport.cellCount = report.cellCount;
    pluginReport.netnameCount = report.netnameCount;
    pluginReport.sha256 = sigflow::platform::Utf8String(report.sha256);

    std::string error;
    const bool ok = validator.WriteManifest(sigflow::platform::Utf8String(manifestPath),
                                            pluginReport, error);
    if (!ok) errorMessage = wxString::FromUTF8(error.c_str());
    return ok;
}
