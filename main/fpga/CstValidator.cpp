// P1-6：CST 文件校验已移入 InnerPlugin/eda-cst-gowin-cst（wx-free）。
// 本文件保留同签名的 wx 适配层，供 NextpnrExecutor / FpgaToolWindow 使用。
#include "CstValidator.h"

#include "CstFileValidator.h"

#include "platform/PlatformPaths.h"

#include <filesystem>
#include <vector>

CstValidator::CstValidator() = default;

wxString CstValidator::AutoResolveCst(const wxString& projectRoot,
                                      const wxString& configuredCstPath) {
    const std::filesystem::path path = eda::cst::CstFileValidator::AutoResolveCst(
        sigflow::platform::Utf8Path(projectRoot),
        sigflow::platform::Utf8String(configuredCstPath));
    return wxString::FromUTF8(sigflow::platform::PathToUtf8(path).c_str());
}

bool CstValidator::ExistsAndReadable(const wxString& cstPath) {
    return eda::cst::CstFileValidator::ExistsAndReadable(sigflow::platform::Utf8String(cstPath));
}

CstValidationResult CstValidator::Validate(const wxString& cstPath,
                                           const std::vector<wxString>& allPorts) {
    std::vector<std::string> ports;
    ports.reserve(allPorts.size());
    for (const auto& port : allPorts) ports.push_back(sigflow::platform::Utf8String(port));

    const eda::cst::CstValidationResult pluginResult =
        eda::cst::CstFileValidator::Validate(sigflow::platform::Utf8String(cstPath), ports);

    CstValidationResult result;
    result.valid = pluginResult.valid;
    result.fileExists = pluginResult.fileExists;
    result.fileNonEmpty = pluginResult.fileNonEmpty;
    result.syntaxOk = pluginResult.syntaxOk;
    result.cstPath = wxString::FromUTF8(pluginResult.cstPath.c_str());
    result.errorSummary = wxString::FromUTF8(pluginResult.errorSummary.c_str());
    for (const auto& lineError : pluginResult.lineErrors) {
        CstValidationResult::LineError converted;
        converted.line = lineError.line;
        converted.content = wxString::FromUTF8(lineError.content.c_str());
        converted.reason = wxString::FromUTF8(lineError.reason.c_str());
        result.lineErrors.push_back(converted);
    }
    for (const auto& port : pluginResult.boundPorts) {
        result.boundPorts.push_back(wxString::FromUTF8(port.c_str()));
    }
    result.totalLines = pluginResult.totalLines;
    result.ioLocCount = pluginResult.ioLocCount;
    result.ioPortCount = pluginResult.ioPortCount;
    result.commentLines = pluginResult.commentLines;
    result.invalidLines = pluginResult.invalidLines;
    return result;
}
