#pragma once
#include <vector>
#include <string>
#include "TreeSitterLinter.h"
#include <optional>

struct StructuringReport {
    struct Suggestion {
        int line;
        int col;
        std::string text;      // 补全的内容，如 "endmodule"
        std::string trigger;   // 触发关键字，如 "module"
    };


    std::string structuredCode;
    std::vector<Suggestion> structuralFixes; // 用于 Tab 补全
};

struct StructeredPackage {
    std::vector<BlockInfo> TSRes;
    std::vector<int> stable_lines;
    std::string stable_code;
    bool is_ts_success = false;
};

struct AnaPac {
    TSNode node;
    bool has_error = false;
};


StructeredPackage VerilogStructuring(const std::string& code);
bool StructureTest(const std::string& code);
AnaPac StructuringX(const std::string& code);

class Structuring {
public:
    void OnInsert(int pos, const std::string& text)
    {
        if (text.empty()) return;

        if (IsEmpty())
        {
            L_ = pos;
            buffer_ = text;
            return;
        }

        // 假设：L_ ≤ pos ≤ R_
        int offset = pos - L_;
        buffer_.insert(offset, text);
    }

    void OnDelete(int pos, int len)
    {
        if (len <= 0 || IsEmpty()) return;

        int R = End();
        int delEnd = pos + len;

        // 删除覆盖全部
        if (pos <= L_ && delEnd >= R)
        {
            buffer_.clear();
            L_ = pos;
            return;
        }

        // 删除完全在内部
        int offset = pos - L_;
        buffer_.erase(offset, len);
    }

    bool IsEmpty() const { return buffer_.empty(); }

    int Start() const { return L_; }

    int End() const { return L_ + static_cast<int>(buffer_.size()); }

    const std::string& Data() const { return buffer_; }

    void Clear()
    {
        buffer_.clear();
        L_ = 0;
    }

private:
    int L_ = 0;
    std::string buffer_;
};
