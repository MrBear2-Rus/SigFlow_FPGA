// P1-6：约束模型/校验/生成逻辑已移入 InnerPlugin/eda-cst-gowin-cst（wx-free）。
// 本文件保留同签名的 wx 适配层，供 FpgaPinBindingPanel / NextpnrExecutor / CstValidator 使用。
#include "FpgaConstraint.h"

#include "ConstraintSheet.h"

#include "platform/PlatformPaths.h"

#include <algorithm>
#include <filesystem>
#include <memory>

namespace {

eda::cst::PortDirection ToPluginDirection(FpgaPortDirection direction) {
    switch (direction) {
        case FpgaPortDirection::Input:  return eda::cst::PortDirection::Input;
        case FpgaPortDirection::Output: return eda::cst::PortDirection::Output;
        case FpgaPortDirection::InOut:  return eda::cst::PortDirection::InOut;
    }
    return eda::cst::PortDirection::Input;
}

FpgaPortDirection FromPluginDirection(eda::cst::PortDirection direction) {
    switch (direction) {
        case eda::cst::PortDirection::Input:  return FpgaPortDirection::Input;
        case eda::cst::PortDirection::Output: return FpgaPortDirection::Output;
        case eda::cst::PortDirection::InOut:  return FpgaPortDirection::InOut;
    }
    return FpgaPortDirection::Input;
}

eda::cst::PinBinding ToPluginBinding(const PinBinding& binding) {
    eda::cst::PinBinding target;
    target.portName = sigflow::platform::Utf8String(binding.portName);
    target.bitIndex = binding.bitIndex;
    target.direction = ToPluginDirection(binding.direction);
    target.packagePin = binding.packagePin;
    target.ioType = sigflow::platform::Utf8String(binding.ioType);
    target.drive = sigflow::platform::Utf8String(binding.drive);
    target.pullUp = binding.pullUp;
    target.pullDown = binding.pullDown;
    target.comment = sigflow::platform::Utf8String(binding.comment);
    target.source = sigflow::platform::Utf8String(binding.source);
    return target;
}

PinBinding FromPluginBinding(const eda::cst::PinBinding& binding) {
    PinBinding target;
    target.portName = wxString::FromUTF8(binding.portName.c_str());
    target.bitIndex = binding.bitIndex;
    target.direction = FromPluginDirection(binding.direction);
    target.packagePin = binding.packagePin;
    target.ioType = wxString::FromUTF8(binding.ioType.c_str());
    target.drive = wxString::FromUTF8(binding.drive.c_str());
    target.pullUp = binding.pullUp;
    target.pullDown = binding.pullDown;
    target.comment = wxString::FromUTF8(binding.comment.c_str());
    target.source = wxString::FromUTF8(binding.source.c_str());
    return target;
}

eda::cst::ConstraintSheet ToPluginSheet(const ConstraintSheet& sheet) {
    eda::cst::ConstraintSheet target;
    target.targetProfileId = sigflow::platform::Utf8String(sheet.targetProfileId);
    target.targetProfileVersion = sigflow::platform::Utf8String(sheet.targetProfileVersion);
    target.topModule = sigflow::platform::Utf8String(sheet.topModule);
    target.device = sigflow::platform::Utf8String(sheet.device);
    target.generatedAt = sigflow::platform::Utf8String(sheet.generatedAt);
    for (const auto& binding : sheet.bindings) target.bindings.push_back(ToPluginBinding(binding));
    return target;
}

ConstraintSheet FromPluginSheet(const eda::cst::ConstraintSheet& sheet) {
    ConstraintSheet target;
    target.targetProfileId = wxString::FromUTF8(sheet.targetProfileId.c_str());
    target.targetProfileVersion = wxString::FromUTF8(sheet.targetProfileVersion.c_str());
    target.topModule = wxString::FromUTF8(sheet.topModule.c_str());
    target.device = wxString::FromUTF8(sheet.device.c_str());
    target.generatedAt = wxString::FromUTF8(sheet.generatedAt.c_str());
    for (const auto& binding : sheet.bindings) target.bindings.push_back(FromPluginBinding(binding));
    return target;
}

ConstraintError FromPluginError(const eda::cst::ConstraintError& error) {
    ConstraintError target;
    target.severity = error.severity == eda::cst::ConstraintError::Severity::Warning
                          ? ConstraintError::Severity::Warning
                          : ConstraintError::Severity::Error;
    target.message = wxString::FromUTF8(error.message.c_str());
    target.field = wxString::FromUTF8(error.field.c_str());
    target.rowIndex = error.rowIndex;
    return target;
}

FpgaPortMap FromPluginPorts(const eda::cst::PortMap& ports) {
    FpgaPortMap result;
    for (const auto& [name, info] : ports) {
        result[wxString::FromUTF8(name.c_str())] = {FromPluginDirection(info.direction), info.width};
    }
    return result;
}

eda::cst::PortMap ToPluginPorts(const FpgaPortMap& ports) {
    eda::cst::PortMap result;
    for (const auto& [name, info] : ports) {
        eda::cst::PortInfo target;
        target.direction = ToPluginDirection(info.first);
        target.width = info.second;
        result[sigflow::platform::Utf8String(name)] = target;
    }
    return result;
}

eda::target::PinDatabase ToPluginPinDatabase(const FpgaPinDatabase& database) {
    eda::target::PinDatabase target;
    target.device = sigflow::platform::Utf8String(database.device);
    target.packageName = sigflow::platform::Utf8String(database.packageName);
    for (const auto& pin : database.pins) {
        eda::target::PinEntry entry;
        entry.pin = pin.pinNumber;
        entry.bank = pin.bank;
        entry.ioLocation = sigflow::platform::Utf8String(pin.ioLocation);
        entry.available = pin.available;
        entry.reservedReason = sigflow::platform::Utf8String(pin.reservedReason);
        entry.defaultIOType = sigflow::platform::Utf8String(pin.defaultIOType);
        target.pins.push_back(std::move(entry));
    }
    for (const auto& resource : database.boardResources) {
        eda::target::BoardResourceEntry entry;
        entry.alias = sigflow::platform::Utf8String(resource.alias);
        entry.pin = resource.packagePin;
        entry.description = sigflow::platform::Utf8String(resource.description);
        entry.type = sigflow::platform::Utf8String(resource.resourceType);
        target.boardResources.push_back(std::move(entry));
    }
    for (const auto& ioType : GetKnownIOTypes()) {
        target.ioTypes.push_back(sigflow::platform::Utf8String(ioType));
    }
    return target;
}

} // namespace

wxString PinBinding::GetFullPortName() const { return portName; }

PinBinding* ConstraintSheet::FindBinding(const wxString& portName, int bitIndex) {
    for (auto& binding : bindings) {
        if (binding.portName == portName && binding.bitIndex == bitIndex) return &binding;
    }
    return nullptr;
}

const PinBinding* ConstraintSheet::FindBinding(const wxString& portName, int bitIndex) const {
    for (const auto& binding : bindings) {
        if (binding.portName == portName && binding.bitIndex == bitIndex) return &binding;
    }
    return nullptr;
}

bool ConstraintSheet::IsPortBound(const wxString& portName, int bitIndex) const {
    const auto* binding = FindBinding(portName, bitIndex);
    return binding != nullptr && binding->packagePin > 0;
}

std::vector<const PinBinding*> ConstraintSheet::GetUnboundPorts() const {
    std::vector<const PinBinding*> result;
    for (const auto& binding : bindings) {
        if (binding.packagePin <= 0) result.push_back(&binding);
    }
    return result;
}

ConstraintValidator::ConstraintValidator(const FpgaPinDatabase& pinDb) : m_pinDb(pinDb) {}

FpgaPortMap ConstraintValidator::ParsePortsFromJsonString(const wxString& jsonStr,
                                                          const wxString& topModule,
                                                          wxString& errorMessage) const {
    const eda::target::PinDatabase database = ToPluginPinDatabase(m_pinDb);
    eda::cst::ConstraintValidator validator(database);
    std::string error;
    const eda::cst::PortMap ports = validator.ParsePortsFromJsonString(
        sigflow::platform::Utf8String(jsonStr), sigflow::platform::Utf8String(topModule), error);
    if (!error.empty()) errorMessage = wxString::FromUTF8(error.c_str());
    return FromPluginPorts(ports);
}

FpgaPortMap ConstraintValidator::ExtractPortsFromYosysJson(const wxString& jsonPath,
                                                           const wxString& topModule,
                                                           wxString& errorMessage) const {
    const eda::target::PinDatabase database = ToPluginPinDatabase(m_pinDb);
    eda::cst::ConstraintValidator validator(database);
    std::string error;
    const eda::cst::PortMap ports = validator.ExtractPortsFromYosysJson(
        sigflow::platform::Utf8String(jsonPath), sigflow::platform::Utf8String(topModule), error);
    if (!error.empty()) errorMessage = wxString::FromUTF8(error.c_str());
    return FromPluginPorts(ports);
}

std::vector<ConstraintError> ConstraintValidator::Validate(const ConstraintSheet& sheet,
                                                           const FpgaPortMap& ports) const {
    const eda::target::PinDatabase database = ToPluginPinDatabase(m_pinDb);
    eda::cst::ConstraintValidator validator(database);
    const std::vector<eda::cst::ConstraintError> errors =
        validator.Validate(ToPluginSheet(sheet), ToPluginPorts(ports));
    std::vector<ConstraintError> result;
    result.reserve(errors.size());
    for (const auto& error : errors) result.push_back(FromPluginError(error));
    return result;
}

std::vector<ConstraintError> ConstraintValidator::ValidateBinding(
    const PinBinding& binding) const {
    const eda::target::PinDatabase database = ToPluginPinDatabase(m_pinDb);
    eda::cst::ConstraintValidator validator(database);
    const std::vector<eda::cst::ConstraintError> errors =
        validator.ValidateBinding(ToPluginBinding(binding));
    std::vector<ConstraintError> result;
    result.reserve(errors.size());
    for (const auto& error : errors) result.push_back(FromPluginError(error));
    return result;
}

CstGenerationResult CstGenerator::Generate(const ConstraintSheet& sheet) const {
    eda::cst::CstGenerator generator;
    const eda::cst::CstGenerationResult pluginResult = generator.Generate(ToPluginSheet(sheet));
    CstGenerationResult result;
    result.success = pluginResult.success;
    result.cstContent = wxString::FromUTF8(pluginResult.cstContent.c_str());
    result.bindingsHash = wxString::FromUTF8(pluginResult.bindingsHash.c_str());
    for (const auto& error : pluginResult.errors) result.errors.push_back(FromPluginError(error));
    return result;
}

std::vector<PinBinding> CstGenerator::ImportCst(const wxString& cstContent,
                                                std::vector<wxString>& unmanagedLines,
                                                wxString& errorMessage) const {
    eda::cst::CstGenerator generator;
    std::vector<std::string> pluginUnmanaged;
    std::string error;
    const std::vector<eda::cst::PinBinding> pluginBindings = generator.ImportCst(
        sigflow::platform::Utf8String(cstContent), pluginUnmanaged, error);
    if (!error.empty()) errorMessage = wxString::FromUTF8(error.c_str());
    unmanagedLines.clear();
    for (const auto& line : pluginUnmanaged) unmanagedLines.push_back(wxString::FromUTF8(line.c_str()));
    std::vector<PinBinding> result;
    result.reserve(pluginBindings.size());
    for (const auto& binding : pluginBindings) result.push_back(FromPluginBinding(binding));
    return result;
}

bool LoadConstraintSheet(const wxString& filePath, ConstraintSheet& sheet, wxString& errorMessage) {
    eda::cst::ConstraintSheet pluginSheet;
    std::string error;
    if (!eda::cst::LoadConstraintSheet(sigflow::platform::Utf8Path(filePath), pluginSheet, error)) {
        errorMessage = wxString::FromUTF8(error.c_str());
        return false;
    }
    sheet = FromPluginSheet(pluginSheet);
    return true;
}

bool SaveConstraintSheet(const wxString& filePath, const ConstraintSheet& sheet,
                         wxString& errorMessage) {
    std::string error;
    if (!eda::cst::SaveConstraintSheet(sigflow::platform::Utf8Path(filePath), ToPluginSheet(sheet),
                                       error)) {
        errorMessage = wxString::FromUTF8(error.c_str());
        return false;
    }
    return true;
}

wxString FindYosysJsonPath(const wxString& projectRoot, const wxString& topModule) {
    const std::filesystem::path path = eda::cst::FindYosysJsonPath(
        sigflow::platform::Utf8Path(projectRoot), sigflow::platform::Utf8String(topModule));
    if (path.empty()) return wxString();
    return wxString::FromUTF8(sigflow::platform::PathToUtf8(path).c_str());
}
