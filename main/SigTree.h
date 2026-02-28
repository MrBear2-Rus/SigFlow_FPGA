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
    Port() = default;
    Port(std::string id, PortDirection dir) :identifier(id), direction(dir) {};
    Port(std::string id, PortDirection dir, std::string conn) :identifier(id), direction(dir), conn(conn) {};
};

enum class GateType {And, Nand, Or, Nor, Xor, Xnor, Buf, Not,Unknown};
enum class SignalType {Wire, Reg, Logic};
enum class EdgeType { Posedge, Negedge};


class SigTreeNode {
private:
    // Tree 连接
    SigTreeNode* parent = nullptr;
    std::vector<SigTreeNode*> children;



    // 父子连接


public:
    SigTreeNodeType type;

    SigTreeNode() = default;
    SigTreeNode(SigTreeNodeType t) : type(t) {};
    virtual ~SigTreeNode() {};

    virtual std::string ToVerilog();
    virtual std::string GetName() { return "Node"; }
    void Print();

    void ClearNode();

    // 父子连接
    virtual bool CanBeChild(SigTreeNodeType childType) const { return true; };
    virtual bool CanBeParent(SigTreeNodeType parentType) const { return true; };
    void RemoveChildren();
    void RemoveChild(SigTreeNode* child);
    bool AddChild(SigTreeNode* child);
    virtual const std::vector<SigTreeNode*>& GetChildren() const {
        return children;
    }
    virtual SigTreeNode* GetParent() {
        return parent;
    }
    void DetachFromParent() { if(parent) parent->RemoveChild(this); };

    // 深克隆到Arena，删除父子关系
    virtual SigTreeNode* Clone(Arena& arena) const = 0;
};

class ProjectNode : public SigTreeNode {
private:


public:
    std::string projectPath; // 来源文件的路径
    ProjectNode(std::string projectPath) : SigTreeNode(SigTreeNodeType::Project), projectPath(projectPath){ };


    void Print();
    std::string ToVerilog() override;
    std::string GetName() override;

    virtual SigTreeNode* Clone(Arena& arena) const override {
        auto* copy = arena.make<ProjectNode>(*this);
        copy->DetachFromParent();
        copy->RemoveChildren(); // 物理连接必须由 Tree 逻辑重新建立
        return copy;
    }

    virtual bool CanBeChild(SigTreeNodeType childType) const override { return childType == SigTreeNodeType::File ? true : false; };
    virtual bool CanBeParent(SigTreeNodeType parentType) const  override { return false; };
};

class FileNode : public SigTreeNode {
private:


public:
    std::string filePath; // 来源文件的路径
    FileNode(std::string filePath):SigTreeNode(SigTreeNodeType::File), filePath(filePath) {};

    void Print();
    std::string ToVerilog() override;
    std::string GetName() override;

    virtual SigTreeNode* Clone(Arena& arena) const override {
        auto* copy = arena.make<FileNode>(*this);
        copy->DetachFromParent();
        copy->RemoveChildren(); // 物理连接必须由 Tree 逻辑重新建立
        return copy;
    }

    virtual bool CanBeChild(SigTreeNodeType childType) const override { return childType == SigTreeNodeType::Top ? true : false; };
    virtual bool CanBeParent(SigTreeNodeType parentType) const override { return parentType == SigTreeNodeType::Project ? true : false; };
};

class TopNode : public SigTreeNode {
private:
    std::vector<Port> in_ports;
    std::vector<Port> out_ports;

public:
    std::string identifier; // 标识符
    TopNodeType topType;

    TopNode(std::string id, TopNodeType toptype):SigTreeNode(SigTreeNodeType::Top), topType(toptype), identifier(id) {};


    std::vector<Port>& GetInPorts() { return in_ports; };
    std::vector<Port>& GetOutPorts() { return out_ports; };
    void UpdateInPorts(std::vector<Port> ps) { in_ports = ps; };
    void UpdateOutPorts(std::vector<Port> ps) { out_ports = ps; };

    void Print();
    std::string ToVerilog() override;
    std::string GetName() override;

    virtual SigTreeNode* Clone(Arena& arena) const override {
        auto* copy = arena.make<TopNode>(*this); // 自动深拷贝 vector<Port>
        copy->DetachFromParent();
        copy->RemoveChildren(); // 物理连接必须由 Tree 逻辑重新建立
        return copy;
    }

    virtual bool CanBeChild(SigTreeNodeType childType) const override { return childType == SigTreeNodeType::Second || childType == SigTreeNodeType::Signal ? true : false; };
    virtual bool CanBeParent(SigTreeNodeType parentType) const override { return parentType == SigTreeNodeType::File ? true : false; };
};


class SignalNode : public SigTreeNode {
private:


public:
    std::string identifier; // 标识符
    SignalType signalType;
    //SignalNode() = default;
    SignalNode(std::string id, SignalType signalType): SigTreeNode(SigTreeNodeType::Signal), identifier(id), signalType(signalType) { };

    void Print();
    std::string ToVerilog() override;
    std::string GetName() override;
    virtual SigTreeNode* Clone(Arena& arena) const override {
        auto* copy = arena.make<SignalNode>(*this);
        copy->DetachFromParent();
        copy->RemoveChildren(); // 物理连接必须由 Tree 逻辑重新建立
        return copy;
    }

    virtual bool CanBeChild(SigTreeNodeType childType) const override { return false; };
    virtual bool CanBeParent(SigTreeNodeType parentType) const override { return parentType == SigTreeNodeType::Top ? true : false; };
};




class SecondNode : public SigTreeNode {
private:


public:
    std::string identifier; // 标识符
    SecondNodeType secondType;

    SecondNode() : SigTreeNode(SigTreeNodeType::Second) {};
    SecondNode(SecondNodeType second) : SigTreeNode(SigTreeNodeType::Second), secondType(second) {};
    SecondNode(std::string id, SecondNodeType second): SigTreeNode(SigTreeNodeType::Second), identifier(id), secondType(second){ };

    std::vector<Port> in_ports;
    std::vector<Port> out_ports;

    virtual std::string ToVerilog() override = 0;
    virtual std::string GetName() override = 0;
    void Print();


    virtual bool CanBeChild(SigTreeNodeType childType) const override { return false; };
    virtual bool CanBeParent(SigTreeNodeType parentType) const override { return parentType == SigTreeNodeType::Top ? true : false; };
};

class GateInstNode : public SecondNode {
public:
    GateType gatetype;

    GateInstNode(std::string id, GateType gt);

    std::string ToVerilog() override;
    std::string GetName() override;
    void Print();

    virtual SigTreeNode* Clone(Arena& arena) const override {
        auto* copy = arena.make<GateInstNode>(*this);
        copy->DetachFromParent();
        copy->RemoveChildren(); // 物理连接必须由 Tree 逻辑重新建立
        // 注意：Definition 指针被拷贝了，这在 Register 阶段会重新校准
        return copy;
    }
};

class ModuleInstNode : public SecondNode {
public:
    std::string defIdentifier;
    TopNode* Definition;
    int magic_check = 0x12345678;
    ModuleInstNode() : SecondNode(SecondNodeType::ModuleInstance) {};
    ModuleInstNode(std::string id, std::string def) :SecondNode(id, SecondNodeType::ModuleInstance), defIdentifier(def) {};
    ModuleInstNode(std::string id, TopNode* Definition) :SecondNode(id, SecondNodeType::ModuleInstance), Definition(Definition) {};

    std::vector<Port> inout_ports;



    std::string ToVerilog() override;
    std::string GetName() override;
    void Print();

    virtual SigTreeNode* Clone(Arena& arena) const override {
        auto* copy = arena.make<ModuleInstNode>(*this);
        copy->DetachFromParent();
        copy->RemoveChildren(); // 物理连接必须由 Tree 逻辑重新建立
        // 注意：Definition 指针被拷贝了，这在 Register 阶段会重新校准
        return copy;
    }
};

class ContinuousAssignNode : public SecondNode {
public:
    std::string template_exp;

    ContinuousAssignNode(std::string id, std::string raw_assign);


    std::string ToVerilog() override;
    std::string GetName() override;
    void Print();

    virtual SigTreeNode* Clone(Arena& arena) const override {
        auto* copy = arena.make<ContinuousAssignNode>(*this);
        copy->DetachFromParent();
        copy->RemoveChildren(); // 物理连接必须由 Tree 逻辑重新建立
        // 注意：Definition 指针被拷贝了，这在 Register 阶段会重新校准
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

class AlwaysNode : public SecondNode {
public:
    EdgeType edgeType;
    std::vector<NB_OR_B_Expression> nb_or_b_expressions;

    AlwaysNode(std::string id, EdgeType edgeType) :SecondNode(id, SecondNodeType::Always), edgeType(edgeType) {};


    std::string ToVerilog() override;
    std::string GetName() override;
    void Print();

    virtual SigTreeNode* Clone(Arena& arena) const override {
        auto* copy = arena.make<AlwaysNode>(*this);
        copy->DetachFromParent();
        copy->RemoveChildren(); // 物理连接必须由 Tree 逻辑重新建立
        // 注意：Definition 指针被拷贝了，这在 Register 阶段会重新校准
        return copy;
    }

    void DelExpressionsPort(int idx, int del);
    void DelExpressionsPort(int del);
    void AddExpressionsPort(int exp_id);
    void AddEmptyExpression();
    void DelLastExpression();
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
    std::map<std::string, ModuleInstNode*> InstanceTable;
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
    void LinkSingleInstWithDef(ModuleInstNode* inst);
    void LinkInstsWithDefs();
    void HangInst(ModuleInstNode* inst);
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
    static std::string ToString(SignalType type) {
        switch (type) {
        case SignalType::Wire:  return "wire";
        case SignalType::Reg:   return "reg";
        case SignalType::Logic: return "logic";
        default: return "wire";
        }
    }
    static std::string ToString(GateType type) {
        switch (type) {
        case GateType::And:  return "and";
        case GateType::Nand: return "nand";
        case GateType::Or:   return "or";
        case GateType::Nor:  return "nor";
        case GateType::Xor:  return "xor";
        case GateType::Xnor: return "xnor";
        case GateType::Buf:  return "buf";
        case GateType::Not:  return "not";
        case GateType::Unknown:
        default:             return "unknown";
        }
    }
    static GateType GateTypeFromString(const std::string& s) {
        // 使用 static 的哈希表，只在第一次调用时初始化，效率最高
        static const std::unordered_map<std::string, GateType> gateMap = {
            {"and",  GateType::And},
            {"nand", GateType::Nand},
            {"or",   GateType::Or},
            {"nor",  GateType::Nor},
            {"xor",  GateType::Xor},
            {"xnor", GateType::Xnor},
            {"buf",  GateType::Buf},
            {"not",  GateType::Not}
        };

        auto it = gateMap.find(s);
        if (it != gateMap.end()) {
            return it->second;
        }

        return GateType::Unknown; // 建议在 enum 中加入 Unknown 类型
    }
};


















