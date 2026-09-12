#pragma once

#include <wx/string.h>
#include <wx/datetime.h>
#include <vector>
#include <map>
#include <utility>
#include "FpgaPinData.h"

struct WxStringLess {
    bool operator()(const wxString& left, const wxString& right) const {
        return left.Cmp(right) < 0;
    }
};

using FpgaPortInfo = std::pair<FpgaPortDirection, int>;
using FpgaPortMap = std::map<wxString, FpgaPortInfo, WxStringLess>;

// ==================== 引脚绑定 ====================
struct PinBinding {
    wxString portName;          // RTL 端口名 (标量: "clk", 向量: "led")
    int bitIndex = 0;           // 向量位索引 (标量端口为 0)
    FpgaPortDirection direction = FpgaPortDirection::Input;
    int packagePin = -1;        // 绑定的物理引脚号, -1 = 未绑定
    wxString ioType = "LVCMOS33"; // IO_TYPE
    wxString drive;             // 驱动强度, e.g. "8", "12", 空字符串表示默认
    bool pullUp = false;
    bool pullDown = false;
    wxString comment;           // 备注 (写入 CST 注释)
    wxString source;            // "gui" / "import" / "auto"

    wxString GetFullPortName() const;  // "led[0]" or "clk"
};

// ==================== 校验错误 ====================
struct ConstraintError {
    enum class Severity { Error, Warning };
    Severity severity = Severity::Error;
    wxString message;
    wxString field;             // 关联的字段路径, e.g. "bindings[3].package_pin"
    int rowIndex = -1;          // bindings 数组索引, -1 表示全局错误

    bool IsError() const { return severity == Severity::Error; }
    bool IsWarning() const { return severity == Severity::Warning; }
};

// ==================== 约束表 ====================
struct ConstraintSheet {
    wxString targetProfileId;   // "tang-nano-9k"
    wxString targetProfileVersion;
    wxString topModule;
    wxString device;            // "GW1NR-LV9QN88PC6/I5"
    std::vector<PinBinding> bindings;
    wxString generatedAt;       // ISO 8601 timestamp

    // 查询
    PinBinding* FindBinding(const wxString& portName, int bitIndex);
    const PinBinding* FindBinding(const wxString& portName, int bitIndex) const;
    bool IsPortBound(const wxString& portName, int bitIndex) const;
    std::vector<const PinBinding*> GetUnboundPorts() const;
};

// ==================== CST 生成结果 ====================
struct CstGenerationResult {
    bool success = false;
    wxString cstContent;
    std::vector<ConstraintError> errors;
    wxString bindingsHash;      // 绑定数据的 SHA-256 hash
};

// ==================== 校验器 ====================
class ConstraintValidator {
public:
    ConstraintValidator(const FpgaPinDatabase& pinDb);

    // 从 Yosys JSON 网表提取端口信息
    // 返回: map<portName, pair<direction, width>>
    FpgaPortMap
        ExtractPortsFromYosysJson(const wxString& jsonPath, const wxString& topModule,
            wxString& errorMessage) const;

    // 从 Yosys JSON 字符串提取端口信息
    FpgaPortMap
        ParsePortsFromJsonString(const wxString& jsonContent, const wxString& topModule,
            wxString& errorMessage) const;

    // 校验完整的 ConstraintSheet
    std::vector<ConstraintError> Validate(const ConstraintSheet& sheet,
        const FpgaPortMap& ports) const;

    // 单个绑定快速校验
    std::vector<ConstraintError> ValidateBinding(const PinBinding& binding) const;

private:
    const FpgaPinDatabase& m_pinDb;
};

// ==================== CST 生成器 ====================
class CstGenerator {
public:
    // 从 ConstraintSheet 生成 CST 文件内容
    CstGenerationResult Generate(const ConstraintSheet& sheet) const;

    // 从 CST 文件导入绑定
    // 返回: vector of PinBinding; 无法解析的行作为 comment 保留
    std::vector<PinBinding> ImportCst(const wxString& cstContent,
        std::vector<wxString>& unmanagedLines,
        wxString& errorMessage) const;

private:
    static wxString ComputeBindingsHash(const std::vector<PinBinding>& bindings);
    static wxString FormatCstHeader(const ConstraintSheet& sheet, const wxString& hash);
};

// ==================== 串行化 ====================
// 读取 pin-bindings.json
bool LoadConstraintSheet(const wxString& filePath, ConstraintSheet& sheet, wxString& errorMessage);

// 写入 pin-bindings.json
bool SaveConstraintSheet(const wxString& filePath, const ConstraintSheet& sheet, wxString& errorMessage);

// 查找 Yosys JSON 网表路径 (从项目根目录)
wxString FindYosysJsonPath(const wxString& projectRoot, const wxString& topModule);
