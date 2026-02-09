#include "LogicBridge.h"
#include <slang/ast/Symbol.h>
#include <slang/ast/symbols/InstanceSymbols.h>
#include <slang/ast/symbols/PortSymbols.h>
#include <slang/ast/symbols/VariableSymbols.h>
#include <slang/ast/symbols/MemberSymbols.h>
#include <slang/ast/symbols/CompilationUnitSymbols.h>

#include <slang/ast/Expression.h>
#include <slang/ast/expressions/AssignmentExpressions.h>
#include <slang/ast/expressions/MiscExpressions.h>

#include <slang/ast/ASTVisitor.h>

#include <slang/text/SourceManager.h>
#include <slang/syntax//AllSyntax.h>


struct VariableScanner : public slang::ast::ASTVisitor<VariableScanner, true, true> {
    std::vector<const slang::ast::Symbol*> symbols;

    void handle(const slang::ast::NamedValueExpression& expr) {
        symbols.push_back(&expr.symbol);
    }
};


LogicBridge::SchematicBuffer LogicBridge::BuildSnapshot(const slang::ast::Compilation& comp) {
    SchematicBuffer buffer;

    const slang::SourceManager* smPtr = comp.getSourceManager();
    if (!smPtr) return buffer;
    const slang::SourceManager& sm = *smPtr;
    auto& nonConstCp = const_cast<slang::ast::Compilation&>(comp);
    const auto& root = nonConstCp.getRoot();
    const slang::ast::Symbol& symol = reinterpret_cast<const slang::ast::Symbol&>(root);
    if (!symol.isScope()) return buffer;
    const slang::ast::Scope& scp = symol.as<slang::ast::Scope>();

    for (const slang::ast::Symbol& sym : scp.members()) {
        if (sym.kind != slang::ast::SymbolKind::Instance) continue;

        std::optional<GraphicTop> topN = BuildTop(sym, sm); // 处理顶层模块自身信息
        if (!topN.has_value()) continue;
        GraphicTop top = topN.value();
        std::vector<GraphicId> pins;
        std::vector<GraphicId> nodes;

        const slang::ast::InstanceSymbol& inst = sym.as<slang::ast::InstanceSymbol>();
        for (const slang::ast::Symbol& sym : inst.body.members()){
            if (auto* port = sym.as_if<slang::ast::PortSymbol>()) { // 顶层模块的端口信息
                GraphicPin pin = BuildPin(*port, sm);
                pins.push_back(pin.id);
                buffer.SigBelongNode.emplace(pin.id, top.id);
                buffer.signals.emplace(pin.id, std::move(pin));

            }
            else if (auto* net = sym.as_if<slang::ast::NetSymbol>()) { // 模块内的信号信息
                GraphicSignal wire = BuildNet(*net, sm);
                buffer.SigBelongNode.emplace(wire.id, top.id);
                buffer.signals.emplace(wire.id, std::move(wire));

            }
            else{
                GraphicNode node;
                node = BuildNode(sym, sm);


                // 按类收集专有知识，在Slang中，任何语法单元都是Symbol，但这里我们只收集需要绘制为节点的Symbol
                if (auto* prim = sym.as_if<slang::ast::PrimitiveInstanceSymbol>()) { //门
                    std::vector<GraphicId> prim_pins;

                    auto connections = prim->getPortConnections();
                    for (size_t i = 0; i < connections.size(); ++i) {
                        auto* conn = connections[i];
                        GraphicPin prim_pin;

                        if (i == 0) {
                            prim_pin.id = std::format("{}.{}", node.id, "out");
                            prim_pin.name = "out";
                            prim_pin.direction = PinDirection::Out;
                        }
                        else {
                            prim_pin.id = std::format("{}.{}{}", node.id, "in", i);
                            prim_pin.name = "in" + std::to_string(i);
                            prim_pin.direction = PinDirection::In;
                        }
                        prim_pin.type = SignalType::Pin;

                        if (const auto* externalSym = conn->getSymbolReference()) {
                            GraphicId signalId = externalSym->getHierarchicalPath();
                            buffer.SigConnections.emplace(prim_pin.id, signalId);
                            buffer.SigConnections.emplace(signalId, prim_pin.id);
                        }

                        prim_pins.push_back(prim_pin.id);
                        buffer.SigBelongNode.emplace(prim_pin.id, node.id);
                        buffer.signals.emplace(prim_pin.id, std::move(prim_pin));
                        

                    }
                    buffer.nodeHasPins.emplace(node.id, prim_pins);
                }
                else if (auto* inst = sym.as_if<slang::ast::InstanceSymbol>()) { //模块实例

                    std::vector<GraphicId> inst_pins;
                    for (auto* conn : inst->getPortConnections()) {
                        GraphicPin inst_pin;
                        const auto& port = conn->port;   // Definition 阶段的 PortSymbol

                        inst_pin.id = conn->port.getHierarchicalPath();


                        inst_pin.name = std::string(port.name);
                        inst_pin.type = SignalType::Pin;

                        const auto& symport = port.as<slang::ast::PortSymbol>();
                        switch (symport.direction) {
                        case slang::ast::ArgumentDirection::In:
                            inst_pin.direction = PinDirection::In;
                            break;
                        case slang::ast::ArgumentDirection::Out:
                            inst_pin.direction = PinDirection::Out;
                            break;
                        case slang::ast::ArgumentDirection::InOut:
                            inst_pin.direction = PinDirection::InOut;
                            break;
                        case slang::ast::ArgumentDirection::Ref:
                            inst_pin.direction = PinDirection::Ref;
                        }
                        inst_pins.push_back(inst_pin.id);

                        if (const auto* expr = conn->getExpression()) {
                            if (const auto* externalSym = expr->getSymbolReference()) {
                                GraphicId signalId = externalSym->getHierarchicalPath();

                                buffer.SigConnections.emplace(inst_pin.id, signalId);
                                buffer.SigConnections.emplace(signalId,  inst_pin.id);
                            }
                        }
                        buffer.SigBelongNode.emplace(inst_pin.id, node.id);
                        buffer.signals.emplace(inst_pin.id, inst_pin);
                    }
                    buffer.nodeHasPins.emplace(node.id, inst_pins);
                    const auto& def = inst->getDefinition();
                    buffer.nodeHasDef.emplace(node.id, def.getHierarchicalPath());

                }
                else if (auto* cas = sym.as_if<slang::ast::ContinuousAssignSymbol>()) { //赋值语句
                    // 将赋值语句作为一个Node，分析其输入输出
                    std::vector<GraphicId> cas_pins;

                    auto& ass = cas->getAssignment();
                    if (auto* assgiment = ass.as_if<slang::ast::AssignmentExpression>()) {
                        const slang::ast::Expression& target = assgiment->left();
                        if (auto output = target.as_if<slang::ast::NamedValueExpression>()) {
                            // 左侧为输出，建为pin
                            GraphicSignal outpin;
                            outpin.id = std::format("{}.{}", node.id, "out");
                            outpin.name = "out";
                            outpin.type = SignalType::Pin;
                            outpin.direction = PinDirection::Out;
                            buffer.signals.emplace(outpin.id, outpin);
                            buffer.SigBelongNode.emplace(outpin.id, node.id);
                            cas_pins.push_back(outpin.id);

                            const slang::ast::Symbol& out_sym = output->symbol;
                            GraphicId signalId = out_sym.getHierarchicalPath();
                            buffer.SigConnections.emplace(outpin.id, signalId);
                            buffer.SigConnections.emplace(signalId, outpin.id);
                        }


                        const slang::ast::Expression& source = assgiment->right();

                        VariableScanner scanner;
                        source.visit(scanner); // 这会自动递归遍历 source 及其所有子节点
                        int count = 1;
                        for (auto in_sym : scanner.symbols) {
                            GraphicSignal inpin;
                            inpin.name = std::format("{}{}","in", count++);;
                            inpin.id = std::format("{}.{}", node.id, inpin.name);
                            inpin.type = SignalType::Pin;
                            inpin.direction = PinDirection::In;
                            buffer.signals.emplace(inpin.id, inpin);
                            buffer.SigBelongNode.emplace(inpin.id, node.id);
                            cas_pins.push_back(inpin.id);

                            GraphicId signalId = in_sym->getHierarchicalPath();
                            buffer.SigConnections.emplace(inpin.id, signalId);
                            buffer.SigConnections.emplace(signalId, inpin.id);

                        }
                        buffer.nodeHasPins.emplace(node.id, cas_pins);



                    }



                }
                else {
                    continue;
                }
                nodes.push_back(node.id);
                buffer.nodes.emplace(node.id, std::move(node));
            }
            
        }
        buffer.topHasNodes.emplace(top.id, nodes);
        buffer.topHasPins.emplace(top.id, pins);

        buffer.tops.emplace(top.id, top);
    }
    
    return buffer;
}


LogicBridge::SourceLocation GetSourceLocation(const slang::ast::Symbol& sym, const slang::SourceManager& sm) {
    auto getSingleLoc = [&](slang::SourceLocation loc) {
        LogicBridge::SourceLocation sl;
        if (!loc) {
            sl.filePath = "<unknown>";
            sl.startLine = -1;
            sl.startCol = -1;
            sl.endLine = -1;
            sl.endCol = -1;
            return sl;
        }
        else {

            loc = sm.getFullyOriginalLoc(loc);
            sl.filePath = std::string(sm.getFileName(loc));
            sl.startLine = sm.getLineNumber(loc);
            sl.startCol = sm.getDisplayColumnNumber(loc);
            sl.endLine = -1;
            sl.endCol = -1;
            return sl;
        }


        };

    LogicBridge::SourceLocation sl;
    if (const auto* syn = sym.getSyntax()) {
        auto range = syn->sourceRange();
        auto startloc = sm.getFullyOriginalLoc(range.start());
        auto endloc = sm.getFullyOriginalLoc(range.end());

        if (startloc && endloc) {
            // end 是 exclusive，GUI 显示通常减 1
            auto endCol = sm.getDisplayColumnNumber(endloc);
            if (endCol > 0)
                endCol--;

            sl.filePath = std::string(sm.getFileName(startloc));
            sl.startLine = sm.getLineNumber(startloc);
            sl.startCol = sm.getDisplayColumnNumber(startloc);
            sl.endLine = sm.getLineNumber(endloc);
            sl.endCol = endCol;
        }
        else {
            sl = getSingleLoc(sym.location);
        }
    }
    else if (auto* inst = sym.as_if<slang::ast::InstanceSymbol>()) {
        const auto* syntax = inst->body.getSyntax();
        auto range = syntax->sourceRange();
        auto startloc = sm.getFullyOriginalLoc(range.start());
        auto endloc = sm.getFullyOriginalLoc(range.end());

        if (startloc && endloc) {
            // end 是 exclusive，GUI 显示通常减 1
            auto endCol = sm.getDisplayColumnNumber(endloc);
            if (endCol > 0)
                endCol--;

            sl.filePath = std::string(sm.getFileName(startloc));
            sl.startLine = sm.getLineNumber(startloc);
            sl.startCol = sm.getDisplayColumnNumber(startloc);
            sl.endLine = sm.getLineNumber(endloc);
            sl.endCol = endCol;
        }
        else {
            sl = getSingleLoc(sym.location);
        }

    }
    else {
        sl = getSingleLoc(sym.location);
    }
    return sl;

}

std::optional<LogicBridge::GraphicTop> LogicBridge::BuildTop(const slang::ast::Symbol& sym, const slang::SourceManager& sm) {
    LogicBridge::GraphicTop top;
    if (sym.kind != slang::ast::SymbolKind::Instance) return std::nullopt;
    
    top.id = sym.getHierarchicalPath();
    if (top.id.find(".") != std::string::npos) return std::nullopt;

    top.name = std::string(sym.name);
    top.typeName = std::string(slang::ast::toString(sym.kind));
    top.sourceLocation = GetSourceLocation(sym, sm);

    const auto* syntax = sym.getSyntax();
    //top.verilogCode = syntax->toString();
    return top;
}


LogicBridge::GraphicNode LogicBridge::BuildNode(const slang::ast::Symbol& sym, const slang::SourceManager& sm) {
    LogicBridge::GraphicNode node;
    node.id = sym.getHierarchicalPath();
    node.name = std::string(sym.name);
    node.typeName = std::string(slang::ast::toString(sym.kind));

    if (auto* prim = sym.as_if<slang::ast::PrimitiveInstanceSymbol>()) { //门
        // 精确化typeName
        if (auto* syntax = prim->getSyntax()) {
            if (syntax->kind == slang::syntax::SyntaxKind::HierarchicalInstance) {
                // 向上看一眼它的父节点，那才是包含 "xor" 关键字的地方
                auto* parent = syntax->parent;
                if (parent && parent->kind == slang::syntax::SyntaxKind::PrimitiveInstantiation) {
                    auto& primSyntax = parent->as<slang::syntax::PrimitiveInstantiationSyntax>();
                    node.typeName = std::string(primSyntax.type.valueText());
                }
                else if (parent && parent->kind == slang::syntax::SyntaxKind::HierarchyInstantiation) {
                    auto& hierSyntax = parent->as<slang::syntax::HierarchyInstantiationSyntax>();
                    node.typeName = std::string(hierSyntax.type.valueText());
                }
            }
        }
    }


    node.sourceLocation = GetSourceLocation(sym, sm);

    const auto* syntax = sym.getSyntax();
    node.verilogCode = syntax->toString();
    if (sym.kind == slang::ast::SymbolKind::ContinuousAssign) {
        node.name = "("+node.verilogCode+")";
        node.id =  node.id +"." + node.name;
    }
    return node;
}

LogicBridge::GraphicPin LogicBridge::BuildPin(const slang::ast::PortSymbol& port, const slang::SourceManager& sm) {
    GraphicPin pin;
    pin.id = port.getHierarchicalPath();
    pin.name = std::string(port.name);
    pin.type = SignalType::Pin;

    switch (port.direction) {
    case slang::ast::ArgumentDirection::In:
        pin.direction = PinDirection::In;
        break;
    case slang::ast::ArgumentDirection::Out:
        pin.direction = PinDirection::Out;
        break;
    case slang::ast::ArgumentDirection::InOut:
        pin.direction = PinDirection::InOut;
        break;
    case slang::ast::ArgumentDirection::Ref:
        pin.direction = PinDirection::Ref;
        break;
    }

    return pin;
}

LogicBridge::GraphicSignal LogicBridge::BuildNet(const slang::ast::NetSymbol& port, const slang::SourceManager& sm) {
    GraphicSignal net;


    net.id = port.getHierarchicalPath();
    net.name = std::string(port.name);
    net.type = SignalType::Net;

    return net;
}









std::string LogicBridge::GetSymbolInfo(const slang::ast::Symbol& symbol, const slang::SourceManager& sm) {
    std::string info;
    info += std::format("Name:{}\n", std::string(symbol.name));
    info += std::format("Path:{}\n", symbol.getHierarchicalPath());
    info += std::format("Slang Kind:{}\n", slang::ast::toString(symbol.kind));


    auto printSingleLoc = [&](slang::SourceLocation loc) {
        if (!loc)
            return std::string("Definition:<unknown>\n");

        loc = sm.getFullyOriginalLoc(loc);
        return std::format("Definition:{} ({}, {})\n",
            std::string(sm.getFileName(loc)),
            sm.getLineNumber(loc),
            sm.getDisplayColumnNumber(loc));
        };

    if (const auto* syn = symbol.getSyntax()) {
        auto range = syn->sourceRange();
        auto startloc = sm.getFullyOriginalLoc(range.start());
        auto endloc = sm.getFullyOriginalLoc(range.end());

        if (startloc && endloc) {
            // end 是 exclusive，GUI 显示通常减 1
            auto endCol = sm.getDisplayColumnNumber(endloc);
            if (endCol > 0)
                endCol--;

            info += std::format("Definition:{} ({}, {})-({}, {})\n",
                std::string(sm.getFileName(startloc)),
                sm.getLineNumber(startloc),
                sm.getDisplayColumnNumber(startloc),
                sm.getLineNumber(endloc),
                endCol);
        }
        else {
            info += printSingleLoc(symbol.location);
        }
    }
    else {
        info += printSingleLoc(symbol.location);
    }
    return info;
}




void LogicBridge::elaborateScope(const slang::ast::Scope& scope, const slang::SourceManager& sm, int& idx) {
    for (auto& member : scope.members()) {
        // 过滤掉不需要记录的中间类型（比如 Root 或 CompilationUnit 本身）
        //if (member.kind == slang::ast::SymbolKind::Root ||
        //    member.kind == slang::ast::SymbolKind::CompilationUnit) {
        //    if (member.isScope()) walkScope(member.as<slang::ast::Scope>(), sm, result);
        //    continue;
        //}
        std::string symbol;
        symbol += std::format("[{}]\n", idx++);
        symbol += GetSymbolInfo(member, sm);
        wxLogDebug("%s", symbol.c_str());



        // 递归进入子作用域（例如 Module 里的 Variable，或 Instance 里的 Port）
        if (member.isScope()) {
            // 如果是普通的 Scope (如 package, module definition)
            elaborateScope(member.as<slang::ast::Scope>(), sm, idx);
        }
        else if (member.kind == slang::ast::SymbolKind::Instance) {
            // 如果是实例 (如 u1, u2)，必须进入它的 body 作用域
            const auto& inst = member.as<slang::ast::InstanceSymbol>();
            elaborateScope(inst.body, sm, idx);
        }
    }
}


void LogicBridge::Elaborate(const slang::ast::Compilation& cp) {
    int i = 0;
    const slang::SourceManager* smPtr = cp.getSourceManager();
    if (!smPtr) return; 
    const slang::SourceManager& sm = *smPtr;
    auto& nonConstCp = const_cast<slang::ast::Compilation&>(cp);
    const auto& root = nonConstCp.getRoot();
    const slang::ast::Symbol& sym = reinterpret_cast<const slang::ast::Symbol&>(root);
    if (sym.isScope()) {
        elaborateScope(sym.as<slang::ast::Scope>(), sm, i);
    }

    return;
}



void LogicBridge::GetDefinitions(const slang::ast::Compilation& cp) {
    int idx = 0;
    const slang::SourceManager* smPtr = cp.getSourceManager();
    if (!smPtr) return;
    const slang::SourceManager& sm = *smPtr;

    auto& nonConstCp = const_cast<slang::ast::Compilation&>(cp);


    for (const slang::ast::Symbol* sym : nonConstCp.getDefinitions()) {
        std::string s = GetSymbolInfo(*sym, sm);
        wxLogDebug("%s", s.c_str());
    }


}


void LogicBridge::printSnapshot(const SchematicBuffer& ss) {
    std::string output;
    output += "=== Schematic Snapshot ===\n";

    // 打印顶层模块
    output += "Top-level modules:\n";
    for (const auto& [id, top] : ss.tops) {
        auto it = ss.tops.find(id);
        if (it != ss.tops.end()) {
            const auto& node = it->second;
            output += std::format("[{}] {} ({}) {} [{}:{}]-[{}:{}]\n",
                node.id,
                node.name,
                node.typeName,
                node.sourceLocation.filePath,
                node.sourceLocation.startLine,
                node.sourceLocation.startCol,
                node.sourceLocation.endLine,
                node.sourceLocation.endCol);
            output += "Pins:\n";
            for (GraphicId pinId : ss.topHasPins.at(node.id)) {
                output += std::format("{}\n",pinId);
            }
            output += "Nodes:\n";
            for (GraphicId nodeId : ss.topHasNodes.at(node.id)) {
                output += std::format("{}\n", nodeId);
            }
            output += std::format("\n");
        }
    }

    // 打印所有节点
    output += "\nAll nodes:\n";
    for (const auto& [id, node] : ss.nodes) {
        output += std::format("Node [{}] \n Name: {}, Type: {}\n", node.id, node.name, node.typeName);

        // 输出逻辑描述
        if (!node.verilogCode.empty())
            output += std::format("  Verilog code: {}\n", node.verilogCode);
        // 输出 pins
        output += "  Pins:\n";
        for (GraphicId pid : ss.nodeHasPins.at(node.id)) {
            auto pit = ss.signals.find(pid);
            if (pit == ss.signals.end()) continue;
            const auto& pin = pit->second;

            output += std::format("    [{}] {} ({})", pin.id, pin.name,
                pin.direction == PinDirection::In ? "In" :
                pin.direction == PinDirection::Out ? "Out" :
                pin.direction == PinDirection::InOut ? "InOut" : "Ref");

            output += "\n";
        }
        output += std::format("{} [{}:{}]-[{}:{}]\n", node.sourceLocation.filePath,
            node.sourceLocation.startLine,
            node.sourceLocation.startCol,
            node.sourceLocation.endLine,
            node.sourceLocation.endCol);

        auto it = ss.nodeHasDef.find(node.id);
        if (it != ss.nodeHasDef.end()) {
            output += std::format("Definition:{}", it->second);
        }
        output += "\n";
    }
    output += "\nAll Signals:\n";
    for (const auto& [id, signal] : ss.signals) {
        output += std::format("{}\n{} {} {}\n",
            signal.id,
            signal.name,
            static_cast<int>(signal.type),
            static_cast<int>(signal.direction));

        auto range = ss.SigConnections.equal_range(signal.id);

        for (auto it = range.first; it != range.second; ++it) {
            GraphicId pinId = it->second; // 这就是其中一个连接的引脚 ID
            output += std::format("Connected to: {}\n", pinId);
        }
        output += "\n";
    }




    wxLogDebug("%s", output.c_str());
}

