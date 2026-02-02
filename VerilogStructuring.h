#pragma once
#include <vector>
#include <string>



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



StructuringReport VerilogStructuring(const std::string& code);




