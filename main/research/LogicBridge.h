#pragma once
#include <unordered_map>
#include <vector>

#include <wx/string.h>
#include <wx/wx.h>

#include <slang/ast/Compilation.h>
#include <slang/ast/symbols/InstanceSymbols.h>
#include <slang/ast/symbols/PortSymbols.h>
#include <slang/ast/symbols/VariableSymbols.h>
#include <slang/ast/symbols/MemberSymbols.h>



namespace LogicBridge {

    using GraphicId = std::string; //用path作为id

    struct SourceLocation {
        std::string filePath = "";
        int startLine = 0, startCol = 0, endLine = 0, endCol = 0;

        SourceLocation() = default;
        SourceLocation(std::string path, int sl, int sc, int el, int ec)
            : filePath(std::move(path)), startLine(sl), startCol(sc), endLine(el), endCol(ec) {
        }
    };


    enum PinDirection { In, Out, InOut, Ref };
    enum SignalType { Pin, Net, Reg };

    struct GraphicSignal {
        GraphicId id = "";
        std::string name = "";
        SignalType type = SignalType::Net;
        PinDirection direction = PinDirection::In;

        GraphicSignal() = default;
        GraphicSignal(GraphicId id, std::string name, SignalType t, PinDirection d)
            : id(std::move(id)), name(std::move(name)), type(t), direction(d) {
        }
    };

    using GraphicPin = GraphicSignal;

    struct GraphicTop {
        GraphicId id = "";
        std::string name = "";
        std::string typeName = "";
        SourceLocation sourceLocation;
        std::string verilogCode = "";

        GraphicTop() = default;
        GraphicTop(GraphicId id, std::string name, std::string typeName, SourceLocation loc, std::string code = "")
            : id(std::move(id)), name(std::move(name)), typeName(std::move(typeName)),
            sourceLocation(std::move(loc)), verilogCode(std::move(code)) {
        }
    };

    struct GraphicNode {
        GraphicId id = "";
        std::string name = "";
        std::string typeName = "";
        SourceLocation sourceLocation;
        std::string verilogCode = "";

        GraphicNode() = default;
        GraphicNode(GraphicId id, std::string name, std::string typeName, SourceLocation loc, std::string code = "")
            : id(std::move(id)), name(std::move(name)), typeName(std::move(typeName)),
            sourceLocation(std::move(loc)), verilogCode(std::move(code)) {
        }
    };

    struct SchematicBuffer {
        std::unordered_map<GraphicId, GraphicTop> tops;
        std::unordered_map<GraphicId, GraphicNode> nodes;
        std::unordered_map<GraphicId, GraphicSignal> signals;

        std::unordered_map<GraphicId, std::vector<GraphicId>> topHasPins;
        std::unordered_map<GraphicId, std::vector<GraphicId>> topHasNodes;

        std::unordered_map<GraphicId, std::vector<GraphicId>> nodeHasPins;
        std::unordered_map<GraphicId, GraphicId> nodeHasDef;

        std::unordered_map<GraphicId, GraphicId> SigBelongNode;
        std::unordered_multimap<GraphicId, GraphicId> SigConnections;
    };



    SchematicBuffer BuildSnapshot(const slang::ast::Compilation& comp);
    GraphicNode BuildNode(const slang::ast::Symbol& sym, const slang::SourceManager& sm);
    std::optional<GraphicTop> BuildTop(const slang::ast::Symbol& sym, const slang::SourceManager& sm);
    GraphicPin BuildPin(const slang::ast::PortSymbol& port, const slang::SourceManager& sm);
    GraphicSignal BuildNet(const slang::ast::NetSymbol& port, const slang::SourceManager& sm);


    void printSnapshot(const SchematicBuffer& ss);



    void Elaborate(const slang::ast::Compilation& cp);
    void elaborateScope(const slang::ast::Scope& scope, const slang::SourceManager& sm, int& idx);
    std::string GetSymbolInfo(const slang::ast::Symbol& symbol, const slang::SourceManager& sm);
    void GetDefinitions(const slang::ast::Compilation& cp);

}
