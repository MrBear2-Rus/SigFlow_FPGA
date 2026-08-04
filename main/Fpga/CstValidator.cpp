// CstValidator.cpp

#include "CstValidator.h"
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/regex.h>
#include <wx/tokenzr.h>
#include <wx/dir.h>
#include <vector>
#include <cstddef>

namespace {

constexpr wxFileOffset kMaxCstFileSize = 100 * 1024;   // CST 文件大小上限: 100KB
constexpr int kMaxGowinPin = 200;                       // GW1NR 全系列最大引脚号

} // anonymous namespace

// 构造函数
CstValidator::CstValidator() { }

// CST 路径自动解析（4 级兜底）
wxString CstValidator::AutoResolveCst(const wxString& projectRoot,
                                       const wxString& configuredCstPath)
{
    // 1. 用户显式 cst= 路径
    if (!configuredCstPath.IsEmpty()) {
        wxFileName fn(configuredCstPath);
        if (fn.IsAbsolute()) return configuredCstPath;

        // 相对路径 -> 基于项目根目录解析
        wxFileName resolved(projectRoot + wxT("\\") + configuredCstPath);
        resolved.MakeAbsolute();
        return resolved.GetFullPath();
    }

    // 2. 默认路径
    const wxString defaultPath =
        wxFileName(projectRoot + wxT("\\constraints\\tangnano9k.cst")).GetFullPath();
    if (wxFile::Exists(defaultPath)) return defaultPath;

    // 3. 扫描 constraints/ 目录
    wxDir constraintsDir(projectRoot + wxT("\\constraints"));
    if (constraintsDir.IsOpened()) {
        wxString filename;
        if (constraintsDir.GetFirst(&filename, wxT("*.cst"), wxDIR_FILES)) {
            return wxFileName(projectRoot + wxT("\\constraints\\") + filename).GetFullPath();
        }
    }

    // 4. 兜底: nextpnr/ 目录
    wxDir nextpnrDir(projectRoot + wxT("\\nextpnr"));
    if (nextpnrDir.IsOpened()) {
        wxString filename;
        if (nextpnrDir.GetFirst(&filename, wxT("*.cst"), wxDIR_FILES)) {
            return wxFileName(projectRoot + wxT("\\nextpnr\\") + filename).GetFullPath();
        }
    }

    return defaultPath;   // 返回默认值，让 Validate 给出有意义的报错
}

// 快速存在+可读检查
bool CstValidator::ExistsAndReadable(const wxString& cstPath)
{
    if (cstPath.IsEmpty()) return false;
    wxFile file(cstPath, wxFile::read);
    if (!file.IsOpened()) return false;
    file.Close();
    return true;
}

// 布线前 CST 4 步校验管线
CstValidationResult CstValidator::Validate(const wxString& cstPath,
                                            const std::vector<wxString>& allPorts)
{
    CstValidationResult result;
    result.cstPath = cstPath;

    // ── Step 1: 文件存在 ──
    result.fileExists = wxFile::Exists(cstPath);
    if (!result.fileExists) {
        result.errorSummary = wxString::Format(
            wxT("CST 约束文件不存在: %s\n请先运行 FPGA > Pin Constraints 生成管脚约束文件。"),
            cstPath);
        return result;
    }

    // ── Step 2: 文件非空 + 大小上限 ──
    {
        wxFile file(cstPath, wxFile::read);
        if (!file.IsOpened()) {
            result.errorSummary = wxT("无法打开 CST 文件。");
            return result;
        }
        const wxFileOffset size = file.Length();
        file.Close();

        if (size <= 0) {
            result.errorSummary = wxT("CST 文件为空，请添加管脚约束后再试。");
            return result;
        }
        if (size > kMaxCstFileSize) {
            result.errorSummary = wxString::Format(
                wxT("CST 文件过大（%.0f KB），可能不是有效的约束文件。最大允许 100KB。"),
                static_cast<double>(size) / 1024.0);
            return result;
        }
        result.fileNonEmpty = true;
    }

    // ── Step 3: 逐行语法校验 ──
    wxString content;
    {
        wxFile readFile(cstPath, wxFile::read);
        if (!readFile.IsOpened()) {
            result.errorSummary = wxT("无法读取 CST 文件。");
            return result;
        }
        // 用 std::vector<char> 替代 new[]/delete[]（避免裸指针管理）
        const wxFileOffset len = readFile.Length();
        std::vector<char> buf(static_cast<std::size_t>(len) + 1);
        readFile.Read(buf.data(), len);
        buf[static_cast<std::size_t>(len)] = '\0';
        content = wxString::FromUTF8(buf.data());
        readFile.Close();
    }

    // 统一换行符
    content.Replace(wxT("\r\n"), wxT("\n"));
    content.Replace(wxT("\r"), wxT("\n"));

    wxStringTokenizer tokenizer(content, wxT("\n"));
    int lineNum = 0;
    while (tokenizer.HasMoreTokens()) {
        wxString line = tokenizer.GetNextToken();
        line.Trim(true).Trim(false);
        ++lineNum;
        ++result.totalLines;
        if (line.IsEmpty()) continue;
        ValidateSingleLine(line, lineNum, result);
    }

    // 汇总
    result.syntaxOk = (result.invalidLines == 0);
    result.valid = result.fileExists && result.fileNonEmpty && result.syntaxOk;

    if (!result.syntaxOk) {
        result.errorSummary = wxString::Format(
            wxT("CST 文件存在 %d 处语法错误（共 %d 行）。\n"),
            result.invalidLines, result.totalLines);
        for (const auto& le : result.lineErrors) {
            result.errorSummary += wxString::Format(
                wxT("  行 %d: %s — %s\n"), le.line, le.content, le.reason);
        }
    }

    // ── Step 4: 端口覆盖检查（可选）──
    if (!allPorts.empty() && result.syntaxOk) {
        std::vector<wxString> unbound;
        for (const auto& port : allPorts) {
            bool found = false;
            for (const auto& bp : result.boundPorts) {
                if (bp == port) { found = true; break; }   // 精确相等
            }
            if (!found) unbound.push_back(port);
        }
        if (!unbound.empty()) {
            result.errorSummary += wxT("\n未绑定的端口: ");
            for (const auto& p : unbound) result.errorSummary += p + wxT(" ");
            result.valid = false;
        }
    }

    return result;
}

// 单行语法校验
bool CstValidator::ValidateSingleLine(const wxString& line, int lineNum,
                                       CstValidationResult& result)
{
    // 注释行
    if (line.StartsWith(wxT("#")) || line.StartsWith(wxT("//"))) {
        ++result.commentLines;
        return true;
    }

    // 静态预编译正则（只构造一次，避免每行重复编译）
    static const wxRegEx ioLocRe(wxT("^IO_LOC\\s+\"([^\"]+)\"\\s+(\\d+)\\s*;"), wxRE_ADVANCED);
    static const wxRegEx ioPortRe(wxT("^IO_PORT\\s+\"([^\"]+)\"\\s+(.+);"), wxRE_ADVANCED);
    static const wxRegEx ioTypeRe(wxT("IO_TYPE\\s*=\\s*(\\w+)"), wxRE_ADVANCED | wxRE_ICASE);

    // IO_LOC "port_name" pin_number;
    if (ioLocRe.Matches(line)) {
        ++result.ioLocCount;
        const wxString portName = ioLocRe.GetMatch(line, 1);
        const wxString pinNum   = ioLocRe.GetMatch(line, 2);
        long pin = 0;
        if (pinNum.ToLong(&pin) && pin > 0 && pin <= kMaxGowinPin) {
            result.boundPorts.push_back(portName);
        } else {
            result.boundPorts.push_back(portName);
            ++result.invalidLines;
            CstValidationResult::LineError le;
            le.line    = lineNum;
            le.content = line;
            le.reason  = wxString::Format(
                wxT("引脚号 \"%s\" 无效——应在 1-%d 范围内"), pinNum, kMaxGowinPin);
            result.lineErrors.push_back(le);
            return false;
        }
        return true;
    }

    // IO_PORT "port_name" IO_TYPE=xxx PULL_MODE=xxx DRIVE=n;
    if (ioPortRe.Matches(line)) {
        ++result.ioPortCount;
        const wxString attrs = ioPortRe.GetMatch(line, 2);
        if (!ioTypeRe.Matches(attrs)) {
            ++result.invalidLines;
            CstValidationResult::LineError le;
            le.line    = lineNum;
            le.content = line;
            le.reason  = wxT("缺少 IO_TYPE 属性（如 IO_TYPE=LVCMOS33）");
            result.lineErrors.push_back(le);
            return false;
        }
        return true;
    }

    // 不匹配任何已知格式
    ++result.invalidLines;
    CstValidationResult::LineError le;
    le.line    = lineNum;
    le.content = line;
    le.reason  = wxT("不支持的 CST 语法——仅接受 IO_LOC / IO_PORT 或 # 注释");
    result.lineErrors.push_back(le);
    return false;
}
