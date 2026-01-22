#pragma once
#include <unordered_map>
#include <vector>

#include <wx/string.h>
#include <wx/wx.h>

#include <slang/ast/Compilation.h>


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













using NodeId = uint64_t;
using PinId = uint64_t;

struct LogicView {
    std::string verilogCode;   // 原始代码片段
    std::string expression;    // 简化后的逻辑等式
    std::string truthTable;    // 如果是 UDP，则存真值表
};

struct SourceLocation {
    std::string filePath;
    int startLine;
    int startCol;
    int endLine;
    int endCol;
};

enum PinDirection {In, Out, InOut, Ref };

struct GraphicPin {
    PinId id;
    std::string name;
    PinDirection direction; // Input=0, Output=1
    std::vector<PinId> connectedPins;
};

struct GraphicNode {
    NodeId id;
    std::string name;
    std::string typeName; // 如 "half_adder" 或 "AND gate"
    SourceLocation sourceLocation; // 原始定义文件，用于跳转
    LogicView logicDescription; // 逻辑表达式或真值表内容

    std::vector<PinId> pins;
};

struct SchematicBuffer {
    std::unordered_map<NodeId, GraphicNode> nodes;
    std::vector<NodeId> topLevelModules;
    std::unordered_map<PinId, GraphicPin> pins;
};


namespace LogicBridge {
    SchematicBuffer BuildSnapshot(const slang::ast::Compilation& comp);
    GraphicNode BuildNode(const slang::ast::Symbol& sym, const slang::SourceManager& sm, NodeId& next_id);
    void ExtractChildren(const slang::ast::Symbol& sym,
        const slang::SourceManager& sm,
        SchematicBuffer& buffer,
        NodeId& node_id,
        PinId& pin_id);
    std::vector<PinId> BuildPort(SchematicBuffer& buffer, const slang::ast::Symbol& sym, PinId& id);



    void printSnapshot(const SchematicBuffer& ss);



    void Elaborate(const slang::ast::Compilation& cp);
    void elaborateScope(const slang::ast::Scope& scope, const slang::SourceManager& sm, int& idx);
    std::string GetSymbolInfo(const slang::ast::Symbol& symbol, const slang::SourceManager& sm);
    void walkDefinitionScope(const slang::ast::Scope& scope, const slang::SourceManager& sm, int& idx);
    void GetDefinitions(const slang::ast::Compilation& cp);

}
