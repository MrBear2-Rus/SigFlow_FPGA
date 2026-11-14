#pragma once
#include <wx/string.h>
#include <wx/datetime.h>
#include "FpgaPinData.h"
#include "ConstraintSheet.h"
#include <eda/api/port_manifest.hpp>
struct WxStringLess { bool operator()(const wxString& a,const wxString& b) const {return a.Cmp(b)<0;} };
using FpgaPortMap = eda::virtual_board::PortManifest;
struct PinBinding {
    eda::virtual_board::PortBitRef ref;
    wxString portName; // full HDL display label; ref is authoritative
    FpgaPortDirection direction = FpgaPortDirection::Input;
    int packagePin = -1;
    wxString ioType, drive;
    bool pullUp = false, pullDown = false;
    wxString comment, source;
    wxString GetFullPortName() const {return portName;}
};
struct ConstraintError {
    enum class Severity {Error, Warning};
    Severity severity = Severity::Error;
    wxString message, field;
    int rowIndex = -1;
    bool IsError() const {return severity==Severity::Error;}
    bool IsWarning() const {return severity==Severity::Warning;}
};
struct ConstraintSheet {
    FpgaPortMap manifest;
    wxString targetProfileId, targetProfileVersion, topModule, device, generatedAt;
    wxString package = "QFN88", boardRevision = "6202";
    std::vector<PinBinding> bindings;
    PinBinding* FindBinding(const wxString& fullLabel);
    const PinBinding* FindBinding(const wxString& fullLabel) const;
};
struct CstGenerationResult {
    bool success = false;
    wxString cstContent, bindingsHash;
    std::vector<ConstraintError> errors;
};
eda::cst::ConstraintSheetV2 ToCoreConstraintSheet(const ConstraintSheet&);
ConstraintSheet FromCoreConstraintSheet(const eda::cst::ConstraintSheetV2&);
std::vector<PinBinding> MakeGuiBindingRows(const FpgaPortMap&);
class ConstraintValidator {
public:
    explicit ConstraintValidator(const FpgaPinDatabase& db):m_pinDb(db){}
    FpgaPortMap ExtractPortsFromYosysJson(const wxString&,const wxString&,wxString&) const;
    std::vector<ConstraintError> Validate(const ConstraintSheet&,const FpgaPortMap&) const;
    std::vector<ConstraintError> ValidateBinding(const PinBinding&,const FpgaPortMap&) const;
private:
    const FpgaPinDatabase& m_pinDb;
};
class CstGenerator {
public:
    CstGenerationResult Generate(const ConstraintSheet&) const;
    std::vector<PinBinding> ImportCst(const wxString&,const FpgaPortMap&,std::vector<wxString>&,wxString&) const;
};
bool LoadConstraintSheet(const wxString&,ConstraintSheet&,wxString&);
bool SaveConstraintSheet(const wxString&,const ConstraintSheet&,wxString&);
wxString FindYosysJsonPath(const wxString&,const wxString&);
