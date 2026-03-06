#include "VerilogStructuring.h"
#include "TreeSitterLinter.h"

#include <unordered_set>
#include <tree_sitter/api.h>
#include <sstream>





extern "C" TSLanguage* tree_sitter_verilog();
std::string get_stable_code(const std::string& code, const std::vector<int>& stable_lines);
std::vector<int> GetStableLines(const std::vector<BlockInfo>& blocks);

StructeredPackage VerilogStructuring(const std::string& code) {
    StructeredPackage sp;
    TreeSitterLinter TSLinter;

    sp.TSRes = TSLinter.Lint(code);
    sp.stable_lines = GetStableLines(sp.TSRes);
    sp.stable_code = get_stable_code(code, sp.stable_lines);
    sp.is_ts_success = true;
    for (auto b : sp.TSRes) {
        if (b.stability != Stability::Stable) sp.is_ts_success = false;
    }
    return sp;
}

bool StructureTest(const std::string& code) {
    TreeSitterLinter TSLinter;
    return TSLinter.TSTest(code);
}

AnaPac StructuringX(const std::string& code) {
    AnaPac sp;
    TreeSitterLinter TSLinter;
    std::tie(sp.node, sp.has_error) = TSLinter.GetStructNode(code);
    if (sp.node.tree) {
    }
    else {
        sp.has_error = true;
    }

    return sp;
}

std::vector<int> GetStableLines(const std::vector<BlockInfo>& blocks) {
    std::vector<int> out;
    std::unordered_set<int> reject;
    for (auto& b : blocks) {
        for (auto& c : b.child_ids) reject.insert(c);
    }

    for (auto& b : blocks) {
        if (reject.find(b.id) == reject.end() && b.stability == Stability::Stable) {
            for (int i = b.start_line; i <= b.end_line; i++) {
                out.push_back(i);
                //OutputDebugStringA(wxString::Format("[%d] ", i));
            }

        }
    }


    return out;
}

std::string get_stable_code(const std::string& code, const std::vector<int>& stable_lines) {
    std::vector<std::string> all_lines;
    std::string line;
    std::istringstream iss(code);

    // 1. 将 code 切分为行
    all_lines.push_back("\n");
    while (std::getline(iss, line)) {
        all_lines.push_back(line);
    }

    // 2. 根据行号拼接（假设 stable_lines 从 0 开始索引）
    std::string stable_code;
    for (int line_num : stable_lines) {
        if (line_num >= 0 && line_num < all_lines.size()) {
            stable_code += all_lines[line_num] + "\n";
        }
    }

    return stable_code;
}
