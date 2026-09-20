#ifndef PLUG_DEEPSEEK_HELPERS_H
#define PLUG_DEEPSEEK_HELPERS_H

#include <wx/string.h>
#include <string>
#include <filesystem>
#include <set>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <cctype>

// Lightweight helper types and functions moved out of Plug_DeepSeek.cpp
struct DSResult {
    wxString analysis;  // 1. 逻辑分析过程
    wxString code;      // 2. 提取出的纯 Verilog 代码
    wxString summary;   // 3. 改动简要总结
    wxString memory;    // 4. 待存储的长期记忆点
};

// Parse AI reply formatted by the plugin's required schema.
DSResult ParseDSResponse(const wxString& raw);

// Helper: create safe filename from raw token
std::string MakeSafeFilename(const std::string& raw, const std::string& defaultExt = ".v");

// 查找解决方案/仓库根目录：从当前工作目录向上查找第一个包含 `.sln` 的目录，找不到则返回当前工作目录
std::string FindSolutionRoot();

// 遍历项目文件并汇总为一个文本块，包含文件名和文件内容。为避免过大，会限制最大字符数和文件数量。
std::string GatherProjectFiles(const std::string& rootPath, size_t maxTotalChars = 150000, size_t maxFiles = 200);

// 在 rootPath 下查找首个包含 Verilog 文件（.v/.sv/.vh/.svh）的子目录，优先返回较浅的目录。
std::string FindVerilogSubdir(const std::string& rootPath);

#endif // PLUG_DEEPSEEK_HELPERS_H
