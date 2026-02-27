#pragma once
#include <string>
#include <vector>
#include <tree_sitter/api.h>
#include <slang/ast/Compilation.h>

#include "CanvasElement.h"

class Arena {
    std::vector<std::unique_ptr<char[]>> blocks;
    size_t blockSize = 1 << 20; // 1MB
    char* cur = nullptr;
    size_t remaining = 0;

public:
    Arena() = default;
    ~Arena() = default;

    void* allocate(size_t n, size_t align = alignof(std::max_align_t)) {
        size_t space = remaining;
        void* p = cur;
        if (!std::align(align, n, p, space)) {
            blocks.emplace_back(std::make_unique<char[]>(blockSize));
            cur = blocks.back().get();
            remaining = blockSize;
            p = cur;
            space = remaining;
            if (!std::align(align, n, p, space))
                throw std::bad_alloc();
        }
        cur = static_cast<char*>(p) + n;
        remaining = space - n;
        return p;
    }

    template <typename T, typename... Args>
    T* make(Args&&... args) {
        void* mem = allocate(sizeof(T), alignof(T));
        return new (mem) T(std::forward<Args>(args)...);
    }

    void reset() {
        blocks.clear();
        cur = nullptr;
        remaining = 0;
    }
};



enum class TopNodeType {
    Module, // 顶层模块
    UDP,
    Null
};

enum class SecondNodeType {
    ModuleInstance, // 二级模块
    ContinuousAssign,
    UDPInstance,
    Register,
    GateInstance,
    If,
    Always,
    Null
};

enum class NetType {
    Wire,
    Reg,
    Logic,
};



enum class SigTreeNodeType {
    Project, // 项目组织
    File,
    Top, // 顶层模块：Module、UDP
    Signal, // 信号：Net、Reg
    Second // 节点：ModuleInstance、ContinuousAssign、UDPInstance、If、Always
};


enum class PortDirection { In, Out, InOut, Ref };

struct Port {
    std::string identifier;
    PortDirection direction;
    std::string conn;

    std::string portDirectionToStr(PortDirection direction) {
        switch (direction){
        case PortDirection::In:
            return "input";
        case PortDirection::Out:
            return "output";
        case PortDirection::InOut:
            return "inout";
        case PortDirection::Ref:
            return "ref";
        default:
            return "unknown";
        }
    };
};

enum class GateType {And, Nand, Or, Nor, Xor, Xnor, Buf, Not};
enum class SignalType {Wire, Reg, Logic};
enum class EdgeType { Posedge, Negedge};

struct VerilogInfo {


    std::string filePath = "";
    std::string Code = ""; // 原始代码
    int startLine = -1, startCol = -1, endLine = -1, endCol = -1;


};


struct SchematicInfo {
    std::complex<double> pos;
    std::complex<double> size;
};








class SigTreeNode {
public:
    SigTreeNodeType type;
    VerilogInfo verilogInfo;
    SchematicInfo schematicInfo;

    // Tree 连接
    SigTreeNode* parent = nullptr;
    std::vector<SigTreeNode*> children;

    virtual ~SigTreeNode() {};
    virtual std::string ToVerilog();
    virtual std::string GetDisplayName() { return "Node"; }
    void PrintSigTreeNode();
    void ClearNode();
    void RemoveChild(SigTreeNode* child);
    void AddChild(SigTreeNode* child);
    virtual SigTreeNode* Clone(Arena& arena) const = 0;
};

class ProjectNode : public SigTreeNode {
public:
    std::string projectPath; // 来源文件的路径
    ProjectNode(std::string projectPath) { this->projectPath = projectPath; type = SigTreeNodeType::Project; };
    void PrintProjectNode();
    std::string ToVerilog() override;
    std::string GetDisplayName() override;
    virtual SigTreeNode* Clone(Arena& arena) const override {
        auto* copy = arena.make<ProjectNode>(*this);
        copy->parent = nullptr;
        copy->children.clear(); // 物理连接必须由 Tree 逻辑重新建立
        return copy;
    }
};

class FileNode : public SigTreeNode {
public:
    std::string filePath; // 来源文件的路径
    FileNode(std::string filePath) { this->filePath = filePath; type = SigTreeNodeType::File; };
    void PrintFileNode();
    std::string ToVerilog() override;
    std::string GetDisplayName() override;
    virtual SigTreeNode* Clone(Arena& arena) const override {
        auto* copy = arena.make<FileNode>(*this);
        copy->parent = nullptr;
        copy->children.clear();
        return copy;
    }
};

class TopNode : public SigTreeNode {
public:
    std::string identifier; // 标识符
    TopNode() { type = SigTreeNodeType::Top; };
    TopNodeType topType;
    void PrintTopNode();
    std::vector<Port> in_ports;
    std::vector<Port> out_ports;
    std::string ToVerilog() override;
    std::string GetDisplayName() override;
    virtual SigTreeNode* Clone(Arena& arena) const override {
        auto* copy = arena.make<TopNode>(*this); // 自动深拷贝 vector<Port>
        copy->parent = nullptr;
        copy->children.clear();
        return copy;
    }
};

struct NB_OR_B_Expression {
    bool is_blocking;
    std::string nb_or_b_expression;
    float delay;
    int out_port_id;
    std::vector<int> in_port_ids;
};

class SecondNode : public SigTreeNode {
public:
    std::string identifier; // 标识符
    SecondNode() { type = SigTreeNodeType::Second; };
    SecondNodeType secondType;
    std::string gatetype;
    void PrintSecondNode();
    std::vector<Port> in_ports;
    std::vector<Port> out_ports;
    std::vector<Port> inout_ports;

    std::string defIdentifier;
    TopNode* Definition;

    std::string assign_expression;
    
    EdgeType edgeType;
    std::vector<NB_OR_B_Expression> nb_or_b_expressions;

    std::string ToVerilog() override;
    std::string GetDisplayName() override ;
    //TopNode* definition;

    std::string ModuleInstanceToVerilog();
    std::string ContinuousAssignToVerilog();
    std::string GateInstanceToVerilog();
    virtual SigTreeNode* Clone(Arena& arena) const override {
        auto* copy = arena.make<SecondNode>(*this);
        copy->parent = nullptr;
        copy->children.clear();
        // 注意：Definition 指针被拷贝了，这在 Register 阶段会重新校准
        return copy;
    }

    void DelExpressionsPort(int del);
    void AddExpressionsPort(int exp_id);
    void AddEmptyExpression();
    void DelLastExpression();
};

class SignalNode : public SigTreeNode {
public:
    std::string identifier; // 标识符
    SignalNode() { type = SigTreeNodeType::Signal; };
    SignalType signalType;
    void PrintSignalNode();
    std::string ToVerilog() override;
    std::string GetDisplayName() override ;
    virtual SigTreeNode* Clone(Arena& arena) const override {
        auto* copy = arena.make<SignalNode>(*this);
        copy->parent = nullptr;
        copy->children.clear();
        return copy;
    }
};






class SigFlowTree {
public:
    Arena arena;
    ProjectNode* root;

    TSQuery* top;
    uint32_t top_error_offset;
    TSQueryError top_error_type;

    TSQuery* second;
    uint32_t second_error_offset;
    TSQueryError second_error_type;

    TSQuery* net;
    uint32_t net_error_offset;
    TSQueryError net_error_type;

    std::map<std::string, TopNode*> DefinitionTable;
    std::map<std::string, SecondNode*> InstanceTable;
    std::map<std::string, SignalNode*> SignalTable;

    SigFlowTree();
    void LoadProject(std::string projectPath);
    void UpdateTreeFromTS(TSTreeCursor* cursor, SigTreeNode* SigRoot, std::string& filePath, std::string& code);
    void UpdateTreeFromSlang(slang::ast::Compilation* compilation);
    void ClearTree();

    // 子树管理
    void RemoveChild(SigTreeNode* parent, SigTreeNode* child);
    SigTreeNode* AddChild(SigTreeNode* parent, SigTreeNode* externalNode);
    SigTreeNode* CloneSubtreeToArena(SigTreeNode* externalNode);


    // 符号表管理
    void RegisterNodeRecursive(SigTreeNode* node);
    void UnregisterNodeRecursive(SigTreeNode* node);
    void LinkSingleInstWithDef(SecondNode* inst);
    void LinkInstsWithDefs();
    void HangInst(SecondNode* inst);
    void ConstructDefinitionTable();
    void ConstructInstanceTable();


    // 节点查询
    FileNode* GetFileNode(std::string filePath);
    static std::vector<int> SecondNodeTopoLevel(TopNode* tn);

    void PrintTree();

    static std::string ToString(TopNodeType type) {
        switch (type) {
        case TopNodeType::Module: return "Module";
        case TopNodeType::UDP:    return "UDP";
        default:                  return "Null";
        }
    }
    static TopNodeType TopNodeTypeFromString(const std::string& s) {
        if (s == "Module") return TopNodeType::Module;
        if (s == "UDP")    return TopNodeType::UDP;
        return TopNodeType::Null;
    }
    static std::string ToString(SecondNodeType type) {
        switch (type) {
        case SecondNodeType::ModuleInstance:   return "Module Instance";
        case SecondNodeType::ContinuousAssign: return "Continuous Assign";
        case SecondNodeType::UDPInstance:      return "UDP Instance";
        case SecondNodeType::Register:         return "Register";
        case SecondNodeType::GateInstance:     return "Gate Instance";
        case SecondNodeType::If:               return "If";
        case SecondNodeType::Always:           return "Always";
        default:                               return "Null";
        }
    }
    static std::string ToString(PortDirection dir) {
        switch (dir) {
        case PortDirection::In:    return "input";
        case PortDirection::Out:   return "output";
        case PortDirection::InOut: return "inout";
        case PortDirection::Ref:   return "ref";
        default:                   return "unknown";
        }
    }
    static std::string ToString(GateType type) {
        switch (type) {
        case GateType::And:  return "and";  case GateType::Nand: return "nand";
        case GateType::Or:   return "or";   case GateType::Nor:  return "nor";
        case GateType::Xor:  return "xor";  case GateType::Xnor: return "xnor";
        case GateType::Buf:  return "buf";  case GateType::Not:  return "not";
        default: return "unknown";
        }
    }
    static std::string ToString(SignalType type) {
        switch (type) {
        case SignalType::Wire:  return "wire";
        case SignalType::Reg:   return "reg";
        case SignalType::Logic: return "logic";
        default: return "wire";
        }
    }
};


















