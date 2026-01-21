#pragma once
#include <slang/ast/Compilation.h>
#include <slang/ast/Symbol.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/ast/ASTVisitor.h>
#include <slang/driver/Driver.h>
#include <unordered_map>
#include <vector>

#include <wx/string.h>
#include <wx/wx.h>

enum class AtomKind {
    Module,
    Interface,
    Program,
    Checker,
    Instance,
    Port,
    Net,
    Variable,
    Parameter,
    Genvar,
    Function,
    Task,
    Block,
    Unknown
};

struct SourceRange {
    std::string file;
    uint32_t startLine, startCol;
    uint32_t endLine, endCol;
};

using AtomID = uint32_t;

struct AtomInfo {
    AtomID id;
    AtomKind kind;

    std::string name;
    std::string fullName;

    SourceRange definition;
    std::vector<SourceRange> uses;

    const slang::ast::Symbol* symbol;   // 反向索引
};


class AtomAnalysis {
public:
    AtomAnalysis() = default;
    slang::driver::Driver driver;
    std::unique_ptr<slang::ast::Compilation> compilation;



    void AnalyzeProject(wxString project_path);
};
