#pragma once
#include <string>
#include <vector>
#include <tree_sitter/api.h>
#include <slang/ast/Compilation.h>


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



enum class SigTreeNodeType {
    Project, // 项目组织
    File,
    Top, // 顶层模块：Module、UDP
    Net, // 信号：Port、Net、Reg
    Second // 节点：ModuleInstance、ContinuousAssign、UDPInstance、If、Always
};

enum class PortDirection { In, Out, InOut, Ref };

struct Port {
    std::string identifier;
    PortDirection direction;
};




struct VerilogInfo {


    std::string filePath = "";
    std::string Code = ""; // 原始代码
    int startLine = -1, startCol = -1, endLine = -1, endCol = -1;


};


struct SchematicInfo {





};








class SigTreeNode {
public:
    SigTreeNodeType type;
    VerilogInfo verilogInfo;
    SchematicInfo schematicInfo;

    // Tree 连接
    SigTreeNode* parent = nullptr;
    std::vector<SigTreeNode*> children;
    void PrintSigTreeNode();
};

class ProjectNode : public SigTreeNode {
public:
    std::string projectPath; // 来源文件的路径
    ProjectNode(std::string projectPath) { projectPath = projectPath; type = SigTreeNodeType::Project; };
    void PrintProjectNode();
};

class FileNode : public SigTreeNode {
public:
    std::string filePath; // 来源文件的路径
    FileNode(std::string filePath) { this->filePath = filePath; type = SigTreeNodeType::File; };
    void PrintFileNode();
};

class TopNode : public SigTreeNode {
public:
    std::string identifier; // 标识符
    TopNode() { type = SigTreeNodeType::Top; };
    TopNodeType topType;
    void PrintTopNode();
    std::vector<Port> ports;
};

class SecondNode : public SigTreeNode {
public:
    std::string identifier; // 标识符
    SecondNode() { type = SigTreeNodeType::Second; };
    SecondNodeType secondType;
    void PrintSecondNode();
    std::vector<Port> ports;
};

class NetNode : public SigTreeNode {
public:
    std::string identifier; // 标识符
    NetNode() { type = SigTreeNodeType::Net; };
    bool isNet = true;
    void PrintNetNode();
};






class SigFlowTree {
public:
    Arena arena;
    ProjectNode* root;

    SigFlowTree() = default;
    SigFlowTree(std::string projectPath);
    void UpdateTreeFromTS(TSTreeCursor* cursor, SigTreeNode* SigRoot, std::string& filePath, std::string& code);
    void UpdateTreeFromSlang(slang::ast::Compilation* compilation);

    void BuildSigSubTree(TSTreeCursor* cursor, SigTreeNode* parentSigNode);
    void PrintTree();
};


















