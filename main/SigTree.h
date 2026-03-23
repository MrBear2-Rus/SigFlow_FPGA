// SigTree.h
#pragma once

#include <string>
#include <vector>
#include <memory>
#include <map>
#include <unordered_map>
#include <queue>
#include <tree_sitter/api.h>
#include <slang/ast/Compilation.h>
#include <wx/event.h>

#include "Statement.h"
#include "CanvasElement.h"


wxDECLARE_EVENT(EVT_SIGFLOWNODE_ADD, wxCommandEvent);
wxDECLARE_EVENT(EVT_SIGFLOWNODE_DEL, wxCommandEvent);
wxDECLARE_EVENT(EVT_SIGFLOWNODE_CHANGED, wxCommandEvent);

class MainFrame;
class AlwaysStatement;

// ====================  Arena  ====================
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

// ====================  Enums  ====================
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

enum class GateType { And, Nand, Or, Nor, Xor, Xnor, Buf, Not, Unknown };
enum class SignalType { Wire, Reg, Logic };
enum class EdgeType { Posedge, Negedge };

// ====================  Port  ====================
struct Port {
    std::string identifier;
    PortDirection direction;
    std::string conn;
    SignalNode* signal = nullptr; 

    static std::string portDirectionToStr(PortDirection direction) {
        switch (direction) {
        case PortDirection::In:    return "input";
        case PortDirection::Out:   return "output";
        case PortDirection::InOut: return "inout";
        case PortDirection::Ref:   return "ref";
        default:                   return "unknown";
        }
    };
    Port() = default;
    Port(std::string id, PortDirection dir) : identifier(id), direction(dir) {};
    Port(std::string id, PortDirection dir, std::string conn) : identifier(id), direction(dir), conn(conn) {};
    Port(std::string id, PortDirection dir, SignalNode* conn);
    void SetSignalTo(SignalNode* sn);
    std::string GetConnectionName() { return conn; };
};


// ====================  StatementSequence 抽象类  ====================
// 语句序列抽象基类，管理一组有序的 Statement
class StatementSequence {
protected:
    std::vector<std::unique_ptr<Statement>> statements_;

public:
    virtual ~StatementSequence() = default;

    // 在末尾添加一条语句
    virtual void addStatement(std::unique_ptr<Statement> stmt) = 0;

    // 在指定位置插入一条语句
    virtual void insertStatement(size_t index, std::unique_ptr<Statement> stmt) = 0;

    // 移除指定位置的语句
    virtual void removeStatement(size_t index) = 0;

    // 获取指定位置的语句（只读）
    virtual const Statement* getStatement(size_t index) const = 0;

    // 获取语句数量
    virtual size_t getStatementCount() const = 0;

    // 遍历所有语句，更新信号名
    virtual void updateSignalName(const std::string& oldName, const std::string& newName) {
        for (auto& stmt : statements_) {
            stmt->updateSignalName(oldName, newName);
        }
    }
};

// ====================  SigTreeNode 基类  ====================
class SigTreeNode {
private:
    SigTreeNode* parent = nullptr;
    std::vector<SigTreeNode*> children;

public:
    SigTreeNodeType type;

    SigTreeNode() = default;
    SigTreeNode(SigTreeNodeType t) : type(t) {};
    virtual ~SigTreeNode() {};

    virtual std::string ToVerilog();
    virtual std::string GetName() { return "Node"; }
    virtual void Print();  // 改为 virtual，以支持派生类的 override

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
    void DetachFromParent() { if (parent) parent->RemoveChild(this); };

    // 深克隆到Arena，删除父子关系
    virtual SigTreeNode* Clone(Arena& arena) const = 0;
};

// ====================  ProjectNode  ====================
class ProjectNode : public SigTreeNode {
public:
    std::string projectPath;

    ProjectNode(std::string projectPath) : SigTreeNode(SigTreeNodeType::Project), projectPath(projectPath) {};

    void Print() override;
    std::string ToVerilog() override;
    std::string GetName() override;

    virtual SigTreeNode* Clone(Arena& arena) const override {
        auto* copy = arena.make<ProjectNode>(*this);
        copy->DetachFromParent();
        copy->RemoveChildren();
        return copy;
    }

    virtual bool CanBeChild(SigTreeNodeType childType) const override { return childType == SigTreeNodeType::File; }
    virtual bool CanBeParent(SigTreeNodeType parentType) const override { return false; }
};

// ====================  FileNode  ====================
class FileNode : public SigTreeNode {
public:
    std::string filePath;

    FileNode(std::string filePath) : SigTreeNode(SigTreeNodeType::File), filePath(filePath) {};

    void Print() override;
    std::string ToVerilog() override;
    std::string GetName() override;

    virtual SigTreeNode* Clone(Arena& arena) const override {
        auto* copy = arena.make<FileNode>(*this);
        copy->DetachFromParent();
        copy->RemoveChildren();
        return copy;
    }

    virtual bool CanBeChild(SigTreeNodeType childType) const override { return childType == SigTreeNodeType::Top; }
    virtual bool CanBeParent(SigTreeNodeType parentType) const override { return parentType == SigTreeNodeType::Project; }
};

// ====================  TopNode  ====================
class TopNode : public SigTreeNode {
public:
    std::string identifier;
    TopNodeType topType;

    TopNode(std::string id, TopNodeType toptype)
        : SigTreeNode(SigTreeNodeType::Top), topType(toptype), identifier(id) {
    };

    std::vector<SignalNode*> in_ports;
    std::vector<SignalNode*> out_ports;
    std::vector<SignalNode*> signals;

    void AddSignal(SignalNode* sn);

    std::vector<SignalNode*>& GetInPorts() { return in_ports; };
    std::vector<SignalNode*>& GetOutPorts() { return out_ports; };
    void UpdateInPorts(std::vector<SignalNode*> ps) { in_ports = ps; };
    void UpdateOutPorts(std::vector<SignalNode*> ps) { out_ports = ps; };
    void AddSecondPort(SecondNode* sn, Port p);
    void SetSecondPortConn(SecondNode* sn, std::string pId, std::string conn);

    void Print() override;
    std::string ToVerilog() override;
    std::string GetName() override;

    virtual SigTreeNode* Clone(Arena& arena) const override {
        auto* copy = arena.make<TopNode>(*this);
        copy->DetachFromParent();
        copy->RemoveChildren();
        return copy;
    }

    virtual bool CanBeChild(SigTreeNodeType childType) const override {
        return childType == SigTreeNodeType::Second || childType == SigTreeNodeType::Signal;
    }
    virtual bool CanBeParent(SigTreeNodeType parentType) const override { return parentType == SigTreeNodeType::File; }
};

// ====================  SignalNode  ====================
class SignalNode : public SigTreeNode{
public:
    std::string identifier;
    SignalType signalType;
    PortDirection direction;

    SignalNode() : SigTreeNode(SigTreeNodeType::Signal) {};
    SignalNode(std::string id, SignalType signalType, PortDirection pd)
        : SigTreeNode(SigTreeNodeType::Signal), identifier(id), signalType(signalType), direction(pd){
    }
    SignalNode(std::string id, SignalType signalType)
        : SigTreeNode(SigTreeNodeType::Signal), identifier(id), signalType(signalType) {
    }
    TopNode* parent = nullptr;

    SignalNode* Clone(Arena& arena) const {
        auto* copy = arena.make<SignalNode>(*this);
         return copy;
    }
    TopNode* GetParent() {
        return parent;
    }
    void Print();
    std::string ToVerilog();
    std::string GetName();
};

// ====================  SecondNode 基类  ====================
class SecondNode : public SigTreeNode {
public:
    std::string identifier;
    SecondNodeType secondType;

    SecondNode() : SigTreeNode(SigTreeNodeType::Second) {};
    SecondNode(SecondNodeType second) : SigTreeNode(SigTreeNodeType::Second), secondType(second) {};
    SecondNode(std::string id, SecondNodeType second)
        : SigTreeNode(SigTreeNodeType::Second), identifier(id), secondType(second) {
    };

    std::vector<Port> in_ports;
    std::vector<Port> out_ports;

    virtual std::string ToVerilog() override = 0;
    virtual std::string GetName() override = 0;
    void Print() override;

    virtual bool CanBeChild(SigTreeNodeType childType) const override { return false; }
    virtual bool CanBeParent(SigTreeNodeType parentType) const override { return parentType == SigTreeNodeType::Top; }
};

// ====================  GateInstNode  ====================
class GateInstNode : public SecondNode {
public:
    GateType gatetype;

    GateInstNode(std::string id, GateType gt);

    std::string ToVerilog() override;
    std::string GetName() override;
    void Print() override;

    virtual SigTreeNode* Clone(Arena& arena) const override {
        auto* copy = arena.make<GateInstNode>(*this);
        copy->DetachFromParent();
        copy->RemoveChildren();
        return copy;
    }
};

// ====================  ModuleInstNode  ====================
class ModuleInstNode : public SecondNode {
public:
    std::string defIdentifier;
    TopNode* Definition;

    ModuleInstNode() : SecondNode(SecondNodeType::ModuleInstance) {};
    ModuleInstNode(std::string id, std::string def) : SecondNode(id, SecondNodeType::ModuleInstance), defIdentifier(def) {};
    ModuleInstNode(std::string id, TopNode* Definition);

    std::vector<Port> inout_ports;

    void SetDefinition(std::string defIdentifier);
    void SetDefinition(TopNode* Definition);

    std::string ToVerilog() override;
    std::string GetName() override;
    void Print() override;

    virtual SigTreeNode* Clone(Arena& arena) const override {
        auto* copy = arena.make<ModuleInstNode>(*this);
        copy->DetachFromParent();
        copy->RemoveChildren();
        return copy;
    }
};

// ====================  ContinuousAssignNode  ====================
class ContinuousAssignNode : public SecondNode {
public:
    std::string template_exp;

    ContinuousAssignNode(std::string id, std::string raw_assign);

    std::string ToVerilog() override;
    std::string GetName() override;
    void Print() override;

    virtual SigTreeNode* Clone(Arena& arena) const override {
        auto* copy = arena.make<ContinuousAssignNode>(*this);
        copy->DetachFromParent();
        copy->RemoveChildren();
        return copy;
    }
};

// ====================  AlwaysNode (继承 StatementSequence)  ====================
class AlwaysNode : public SecondNode, public StatementSequence {
public:
    EdgeType edgeType;

    AlwaysNode(std::string id, EdgeType edgeType)
        : SecondNode(id, SecondNodeType::Always), edgeType(edgeType) {
    }

    // ----- StatementSequence 接口实现 -----
    void addStatement(std::unique_ptr<Statement> stmt) override;
    void insertStatement(size_t index, std::unique_ptr<Statement> stmt) override;
    void removeStatement(size_t index) override;
    const Statement* getStatement(size_t index) const override;
    size_t getStatementCount() const override;
    void updateSignalName(const std::string& oldName, const std::string& newName) override;

    // ----- 新增的表达式和端口操作方法 -----
    void AddEmptyExpression();                     // 添加一个空语句，自动生成输出端口
    void DelLastExpression();                       // 删除最后一个语句
    void AddPortToExpression(int exp_id);           // 为指定表达式添加输入端口
    void AddPortToExpression(AlwaysStatement* stmt); // 基于语句指针的版本
    void DeletePort(int portIndex);                 // 删除指定索引的输入端口（全局）
    void RemoveExpression(size_t index);            // 删除指定索引的表达式

    std::string ToVerilog() override;
    std::string GetName() override;
    void Print() override;

    void CleanUnusedInPorts();   // 删除未被任何语句引用的输入端口

    void DeletePort(const std::string& portName);
    SigTreeNode* Clone(Arena& arena) const override;
};

// ====================  SigFlowTree  ====================
class SigFlowTree {
public:
    MainFrame* m_parent;

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

    SigFlowTree(MainFrame* parent);
    void LoadProject(std::string projectPath);
    void UpdateTreeFromTS(TSTreeCursor* cursor, SigTreeNode* SigRoot, std::string& filePath, std::string& code, std::unordered_map<SigTreeNode*, std::tuple<int, int>>& outMap);
    void UpdateTreeFromSlang(slang::ast::Compilation* compilation);
    void ClearTree();

    // 子树管理
    void RemoveChild(SigTreeNode* parent, SigTreeNode* child);
    SigTreeNode* AddChild(SigTreeNode* parent, SigTreeNode* externalNode);
    SignalNode* AddSignal(TopNode* parent, SignalNode* sn);
    void RemoveSignal(TopNode* parent, SignalNode* sn);
    SignalNode* AddNewWire(TopNode* parent);
    SigTreeNode* CloneSubtreeToArena(SigTreeNode* externalNode);
    void AddInPort(SecondNode* sn);
    void AddOutPort(SecondNode* sn);
    void AddInOutPort(SecondNode* sn);
    void SecondDelLastInPort(SecondNode* sn);
    void AddInPort(TopNode* tn);
    void AddOutPort(TopNode* tn);
    void TopDelPort(TopNode* sn, Port p);
    void TopDelPort(TopNode* sn, wxString port_id);
    void PortReName(TopNode* tn, wxString old_id, wxString new_id);
    void PortReName(SecondNode* sn, wxString old_id, wxString new_id);
    void PortConn(SecondNode* sn, wxString id, wxString conn);
    void ReIdentifier(TopNode* tn, wxString id);
    void ReIdentifier(SecondNode* sn, wxString id);
    void ReIdentifier(SignalNode* sn, wxString id);

    // 符号表管理
    void RegisterNodeRecursive(SigTreeNode* node);
    void UnregisterNodeRecursive(SigTreeNode* node);
    void LinkSingleInstWithDef(ModuleInstNode* inst);
    void LinkInstsWithDefs();
    void HangInst(ModuleInstNode* inst);
    void ConstructDefinitionTable();
    void ConstructInstanceTable();
    std::vector<std::string> GetDefinitions();

    // 节点查询
    FileNode* GetFileNode(std::string filePath);
    static std::vector<int> SecondNodeTopoLevel(TopNode* tn);

    void PrintTree();

    // 类型转换辅助函数
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
        if (it != gateMap.end()) return it->second;
        return GateType::Unknown;
    }
};

// ====================  AlwaysStatement  ====================
class AlwaysStatement : public Statement {
public:
    bool is_blocking;                     // true for blocking (=), false for non-blocking (<=)
    std::string nb_or_b_expression;        // RHS 表达式模板字符串
    float delay;                           // 可选延迟值
    std::string out_port_name;              // LHS 变量名（输出端口）
    std::vector<std::string> in_port_names; // RHS 中出现的所有输入信号名

    AlwaysStatement() = default;
    AlwaysStatement(bool blocking, const std::string& expr, float dly,
        const std::string& out, const std::vector<std::string>& in)
        : is_blocking(blocking), nb_or_b_expression(expr), delay(dly),
        out_port_name(out), in_port_names(in) {
    }

    // Statement 接口实现
    std::vector<std::string> getReadSignalNames() const override {
        return in_port_names;
    }

    std::vector<std::string> getWriteSignalNames() const override {
        return { out_port_name };
    }

    std::unique_ptr<Statement> clone() const override {
        return std::make_unique<AlwaysStatement>(*this);
    }

    void updateSignalName(const std::string& oldName, const std::string& newName) override {
        if (out_port_name == oldName) out_port_name = newName;
        for (auto& name : in_port_names) {
            if (name == oldName) name = newName;
        }
    }

    AssignmentType getAssignmentType() const override {
        return is_blocking ? AssignmentType::BLOCKING : AssignmentType::NONBLOCKING;
    }

};
