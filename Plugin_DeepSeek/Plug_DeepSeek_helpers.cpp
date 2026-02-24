#include "pch.h"
#include "Plug_DeepSeek_helpers.h"
#include <nlohmann/json.hpp>
#include <sstream>

using json = nlohmann::json;

DSResult ParseDSResponse(const wxString& raw) {
    DSResult res;

    wxString tag1 = wxString::FromUTF8("## 1. 分析");
    wxString tag2 = wxString::FromUTF8("## 2. 纯代码");
    wxString tag3 = wxString::FromUTF8("## 3. 简要总结");
    wxString tag4 = wxString::FromUTF8("## 4. 记忆存储");

    int pos1 = raw.Find(tag1);
    int pos2 = raw.Find(tag2);
    int pos3 = raw.Find(tag3);
    int pos4 = raw.Find(tag4);

    if (pos1 != wxNOT_FOUND && pos2 != wxNOT_FOUND) {
        int start = pos1 + tag1.Length();
        res.analysis = raw.Mid(start, pos2 - start).Trim(true).Trim(false);
    }

    if (pos2 != wxNOT_FOUND && pos3 != wxNOT_FOUND) {
        int start = pos2 + tag2.Length();
        wxString rawCodePart = raw.Mid(start, pos3 - start).Trim(true).Trim(false);
        rawCodePart.Replace("```verilog", "");
        rawCodePart.Replace("```", "");
        res.code = rawCodePart.Trim(true).Trim(false);
    }

    if (pos3 != wxNOT_FOUND && pos4 != wxNOT_FOUND) {
        int start = pos3 + tag3.Length();
        res.summary = raw.Mid(start, pos4 - start).Trim(true).Trim(false);
    }

    if (pos4 != wxNOT_FOUND) {
        int start = pos4 + tag4.Length();
        res.memory = raw.Mid(start).Trim(true).Trim(false);
    }

    return res;
}

std::string MakeSafeFilename(const std::string& raw, const std::string& defaultExt) {
    if (raw.empty()) return std::string("untitled") + defaultExt;
    size_t s = 0, e = raw.size();
    while (s < e && isspace((unsigned char)raw[s])) ++s;
    while (e > s && isspace((unsigned char)raw[e-1])) --e;
    std::string name = raw.substr(s, e - s);

    if ((name.size() >= 2) && ((name.front() == '"' && name.back() == '"') || (name.front() == '\'' && name.back() == '\'' ) || (name.front() == '`' && name.back() == '`'))) {
        name = name.substr(1, name.size()-2);
    }

    for (auto &c : name) if (c == '\\') c = '/';
    size_t lastSlash = name.find_last_of('/');
    std::string base = (lastSlash == std::string::npos) ? name : name.substr(lastSlash + 1);

    std::string out;
    for (size_t i = 0; i < base.size(); ++i) {
        char c = base[i];
        if (isalnum((unsigned char)c) || c == '_' || c == '-' || c == '$' || c == '.') {
            out.push_back(c);
        } else if (isspace((unsigned char)c) || c == ':' || c == '/') {
            out.push_back('_');
        } else {
            out.push_back('_');
        }
    }

    if (out.empty()) out = "untitled";
    if (isdigit((unsigned char)out[0])) out = std::string("m_") + out;

    size_t dot = out.find_last_of('.');
    std::string nameNoExt = (dot == std::string::npos) ? out : out.substr(0, dot);
    std::string ext = (dot == std::string::npos) ? std::string() : out.substr(dot);
    for (auto &c : nameNoExt) if (isupper((unsigned char)c)) c = (char)tolower(c);
    for (auto &c : ext) if (isupper((unsigned char)c)) c = (char)tolower(c);

    if (ext.empty()) ext = defaultExt;
    return nameNoExt + ext;
}

std::string FindSolutionRoot() {
    namespace fs = std::filesystem;
    fs::path p = fs::current_path();
    while (true) {
        try {
            for (auto &entry : fs::directory_iterator(p)) {
                if (entry.is_regular_file() && entry.path().extension() == ".sln") return p.string();
                if (entry.is_regular_file() && entry.path().filename() == "sigflow.project") return p.string();
                if (entry.is_directory() && entry.path().filename() == ".sigflow") return p.string();
            }
        }
        catch (...) {
        }
        if (p.has_parent_path()) p = p.parent_path();
        else break;
    }
    return std::filesystem::current_path().string();
}

std::string GatherProjectFiles(const std::string& rootPath, size_t maxTotalChars, size_t maxFiles) {
    namespace fs = std::filesystem;
    std::string result;
    size_t total = 0;
    size_t count = 0;

    std::set<std::string> ignoreDirs = {".git", "build", "bin", "obj", ".vs", "Debug", "Release"};
    std::set<std::string> allowedExt = {".cpp",".c",".h",".hpp",".txt",".md",".py",".cs",".java",".json",".xml",".sln",".vcxproj",".vcxproj.filters",".rc",".yml",".yaml",".ini",".cmake",".makefile",".pro",".project",".v",".sv",".vh",".svh"};

    try {
        for (auto it = fs::recursive_directory_iterator(rootPath); it != fs::recursive_directory_iterator(); ++it) {
            if (count >= maxFiles || total >= maxTotalChars) break;
            try {
                const auto& p = it->path();
                if (p.has_filename()) {
                    std::string fname = p.filename().string();
                    for (const auto& ig : ignoreDirs) {
                        if (fname == ig) { it.disable_recursion_pending(); goto cont; }
                    }
                }
                if (!it->is_regular_file()) { cont: ; continue; }
                std::string ext = p.extension().string();
                for (auto &ch : ext) ch = (char)tolower(ch);
                if (allowedExt.find(ext) == allowedExt.end()) continue;
                std::ifstream ifs(p, std::ios::in | std::ios::binary);
                if (!ifs) continue;
                std::stringstream ss;
                ss << ifs.rdbuf();
                std::string content = ss.str();
                const size_t maxPerFile = 20000;
                if (content.size() > maxPerFile) content = content.substr(0, maxPerFile) + "\n...<truncated>...\n";
                std::string header = "==== " + p.string() + " ====\n";
                if (total + header.size() + content.size() > maxTotalChars) break;
                result += header;
                result += content + "\n\n";
                total += header.size() + content.size();
                ++count;
            }
            catch (...) { continue; }
        }
    }
    catch (...) { }

    if (result.empty()) result = "<No project files collected>";
    return result;
}

std::string FindVerilogSubdir(const std::string& rootPath) {
    namespace fs = std::filesystem;
    std::vector<std::string> verilogExts = {".v", ".sv", ".vh", ".svh"};
    try {
        for (auto it = fs::recursive_directory_iterator(rootPath); it != fs::recursive_directory_iterator(); ++it) {
            try {
                if (!it->is_directory()) continue;
                const auto dir = it->path();
                size_t found = 0;
                for (auto& entry : fs::directory_iterator(dir)) {
                    if (!entry.is_regular_file()) continue;
                    std::string ext = entry.path().extension().string();
                    for (auto &ch : ext) ch = (char)tolower(ch);
                    for (const auto& ve : verilogExts) {
                        if (ext == ve) { ++found; break; }
                    }
                    if (found) break;
                }
                if (found) return dir.string();
            }
            catch (...) { continue; }
        }
    }
    catch (...) { }
    return std::string();
}
