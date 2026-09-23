// P1-3：pack 记账实现已移入 InnerPlugin/eda-pack-gowin（wx-free）。
// 本文件保留同签名的 wx 适配层，供 MainFrame 在迁移期使用。
#include "FpgaPackService.h"

#include "GowinPackService.h"

#include "platform/PlatformPaths.h"

namespace {

eda::pack::PackRequest ToPluginRequest(const FpgaPackRequest& request) {
    eda::pack::PackRequest target;
    target.pnrJsonPath = sigflow::platform::Utf8String(request.pnrJsonPath);
    target.bitstreamPath = sigflow::platform::Utf8String(request.bitstreamPath);
    target.executablePath = sigflow::platform::Utf8String(request.executablePath);
    target.device = sigflow::platform::Utf8String(request.device);
    return target;
}

eda::pack::PackReport ToPluginReport(const FpgaPackReport& report) {
    eda::pack::PackReport target;
    target.success = report.success;
    target.exitCode = report.exitCode;
    target.message = sigflow::platform::Utf8String(report.message);
    target.completedAt = sigflow::platform::Utf8String(report.completedAt);
    target.inputPnrJsonPath = sigflow::platform::Utf8String(report.inputPnrJsonPath);
    target.inputPnrJsonSizeBytes = report.inputPnrJsonSizeBytes;
    target.inputPnrJsonSha256 = sigflow::platform::Utf8String(report.inputPnrJsonSha256);
    target.bitstreamPath = sigflow::platform::Utf8String(report.bitstreamPath);
    target.bitstreamSizeBytes = report.bitstreamSizeBytes;
    target.bitstreamSha256 = sigflow::platform::Utf8String(report.bitstreamSha256);
    target.executablePath = sigflow::platform::Utf8String(report.executablePath);
    target.executableSha256 = sigflow::platform::Utf8String(report.executableSha256);
    target.device = sigflow::platform::Utf8String(report.device);
    return target;
}

void ApplyPluginReport(const eda::pack::PackReport& source, FpgaPackReport& target) {
    target = FpgaPackReport();
    target.success = source.success;
    target.exitCode = source.exitCode;
    target.message = wxString::FromUTF8(source.message.c_str());
    target.completedAt = wxString::FromUTF8(source.completedAt.c_str());
    target.inputPnrJsonPath = wxString::FromUTF8(source.inputPnrJsonPath.c_str());
    target.inputPnrJsonSizeBytes = source.inputPnrJsonSizeBytes;
    target.inputPnrJsonSha256 = wxString::FromUTF8(source.inputPnrJsonSha256.c_str());
    target.bitstreamPath = wxString::FromUTF8(source.bitstreamPath.c_str());
    target.bitstreamSizeBytes = source.bitstreamSizeBytes;
    target.bitstreamSha256 = wxString::FromUTF8(source.bitstreamSha256.c_str());
    target.executablePath = wxString::FromUTF8(source.executablePath.c_str());
    target.executableSha256 = wxString::FromUTF8(source.executableSha256.c_str());
    target.device = wxString::FromUTF8(source.device.c_str());
}

} // namespace

bool FpgaPackService::ValidateInput(const wxString& pnrJsonPath, wxString& errorMessage) const {
    std::string error;
    const bool ok = eda::pack::PackService().ValidateInput(
        sigflow::platform::Utf8String(pnrJsonPath), error);
    if (!ok) errorMessage = wxString::FromUTF8(error.c_str());
    return ok;
}

bool FpgaPackService::Finalize(const FpgaPackRequest& request, int exitCode,
                               FpgaPackReport& report) const {
    eda::pack::PackReport pluginReport;
    const bool ok = eda::pack::PackService().Finalize(ToPluginRequest(request), exitCode, pluginReport);
    ApplyPluginReport(pluginReport, report);
    return ok;
}

bool FpgaPackService::WriteManifest(const wxString& manifestPath, const FpgaPackReport& report,
                                    wxString& errorMessage) const {
    std::string error;
    const bool ok = eda::pack::PackService().WriteManifest(
        sigflow::platform::Utf8String(manifestPath), ToPluginReport(report), error);
    if (!ok) errorMessage = wxString::FromUTF8(error.c_str());
    return ok;
}
