// CstValidator.h
#pragma once

#include <wx/string.h>
#include <wx/arrstr.h>
#include <vector>

// CST 校验结果
struct CstValidationResult {
    bool valid = false;            // 全部校验通过（四步全过）
    bool fileExists = false;       // 文件存在
    bool fileNonEmpty = false;     // 文件非空
    bool syntaxOk = false;         // 语法无错误
    wxString cstPath;              // 实际使用的 CST 绝对路径
    wxString errorSummary;         // 用户可读的错误摘要（多行）

    // 逐行错误详情
    struct LineError {
        int line = 0;
        wxString content;
        wxString reason;
    };
    std::vector<LineError> lineErrors;

    // 已绑定的端口名列表（从 IO_LOC 行提取）
    std::vector<wxString> boundPorts;

    // 统计
    int totalLines = 0;
    int ioLocCount = 0;
    int ioPortCount = 0;
    int commentLines = 0;
    int invalidLines = 0;
};

// CST 约束文件校验器
// 提供两级能力：
//   1. AutoResolveCst(): 自动搜索项目中的 CST 文件（4 级兜底）
//   2. Validate(): 4 步校验管线（文件存在 → 非空 → 逐行语法 → 端口覆盖）
class CstValidator
{
public:
    CstValidator();

    // 自动解析 CST 路径：
    //   1. 用户显式 cst= 路径（绝对/相对）
    //   2. constraints/tangnano9k.cst
    //   3. constraints/*.cst（取第一个）
    //   4. nextpnr/*.cst（取第一个）
    static wxString AutoResolveCst(const wxString& projectRoot,
                                    const wxString& configuredCstPath);

    // 布线前 CST 4 步校验
    // cstPath:  待校验文件的绝对路径
    // allPorts: 顶层模块端口名列表（可选，传入则做端口覆盖检查）
    static CstValidationResult Validate(const wxString& cstPath,
                                         const std::vector<wxString>& allPorts = {});

    // 快速检查是否存在且可读
    static bool ExistsAndReadable(const wxString& cstPath);

private:
    // 单行校验：检查是否符合 IO_LOC / IO_PORT / 注释语法
    static bool ValidateSingleLine(const wxString& line, int lineNum,
                                    CstValidationResult& result);
};
