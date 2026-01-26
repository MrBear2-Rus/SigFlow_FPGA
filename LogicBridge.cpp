#include "LogicBridge.h"
#include <slang/ast/Symbol.h>
#include <slang/ast/symbols/InstanceSymbols.h>
#include <slang/ast/symbols/PortSymbols.h>
#include <slang/ast/symbols/VariableSymbols.h>
#include <slang/ast/symbols/MemberSymbols.h>
#include <slang/text/SourceManager.h>
#include <slang/syntax//AllSyntax.h>

SchematicBuffer LogicBridge::BuildSnapshot(const slang::ast::Compilation& comp) {
    SchematicBuffer buffer;
    NodeId ni = 0;
    PinId pi = 0;

    const slang::SourceManager* smPtr = comp.getSourceManager();
    if (!smPtr) return buffer;
    const slang::SourceManager& sm = *smPtr;
    auto& nonConstCp = const_cast<slang::ast::Compilation&>(comp);
    const auto& root = nonConstCp.getRoot();
    const slang::ast::Symbol& symol = reinterpret_cast<const slang::ast::Symbol&>(root);
    if (!symol.isScope()) return buffer;
    const slang::ast::Scope& scp = symol.as<slang::ast::Scope>();

    for (const slang::ast::Symbol& sym : scp.members()) {
        auto* inst = sym.as_if<slang::ast::InstanceSymbol>();
        if (!inst) continue;

        GraphicNode top = BuildNode(inst->body, sm, ni);
        top.pins = BuildPort(buffer, inst->body, pi);
        NodeId topId = top.id;

        buffer.nodes.emplace(topId, std::move(top));
        buffer.topLevelModules.push_back(topId);

        ExtractChildren(inst->body, sm, buffer,ni,pi);
    }

    return buffer;
}


SourceLocation GetSourceLocation(const slang::ast::Symbol& sym, const slang::SourceManager& sm) {
    auto getSingleLoc = [&](slang::SourceLocation loc) {
        SourceLocation sl;
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

    SourceLocation sl;
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
    else {
        sl = getSingleLoc(sym.location);
    }
    return sl;

}

GraphicNode LogicBridge::BuildNode(const slang::ast::Symbol& sym, const slang::SourceManager& sm, NodeId& node_id) {
    GraphicNode node;
    node.id = node_id++;

    node.name = std::string(sym.name);
    node.typeName = std::string(slang::ast::toString(sym.kind));
    node.sourceLocation = GetSourceLocation(sym, sm);
    // logicDescription

    return node;
}

std::vector<PinId> LogicBridge::BuildPort(SchematicBuffer& buffer, const slang::ast::Symbol& sym, PinId& id) {
    std::vector<PinId> pins_ids;
    if (!sym.isScope()) return pins_ids;

    const auto& scope = sym.as<slang::ast::Scope>();
    for (const auto& member : scope.members()) {
        if (member.kind == slang::ast::SymbolKind::Port) {
            const auto& port = member.as<slang::ast::PortSymbol>();

            GraphicPin pin;

            pin.id = id++;
            pin.name = std::string(port.name);

            // 3. 映射方向
            // Slang 定义了 PortDirection 枚举：In, Out, InOut
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
            }

            // 至于 connectedPins，目前不一定每个pin都有自己的id，因此暂不收集

            buffer.pins.emplace(pin.id ,std::move(pin));
            pins_ids.push_back(pin.id);
        }
    }
    return pins_ids;
}






void LogicBridge::ExtractChildren(const slang::ast::Symbol& sym,
    const slang::SourceManager& sm,
    SchematicBuffer& buffer,
    NodeId& node_id,
    PinId& pin_id) {
    if (!sym.isScope()) return;
    const slang::ast::Scope& scope = sym.as<slang::ast::Scope>();

    for (const slang::ast::Symbol& member : scope.members()) {
        if (auto* inst = member.as_if<slang::ast::InstanceSymbol>()) {
            GraphicNode node;
            node.id = node_id++;
            node.name = std::string(inst->name);
            node.typeName = std::string(slang::ast::toString(inst->kind));

            node.sourceLocation = GetSourceLocation(*inst, sm);

            // logicDescription



            // 收集Pins以及
            std::vector<PinId> pins;
            for (auto* conn : inst->getPortConnections()) {
                GraphicPin pin;
                pin.id = pin_id++;
                const auto& port = conn->port;   // Definition 阶段的 PortSymbol

                pin.name = std::string(port.name);

                const auto& symport = port.as<slang::ast::PortSymbol>();
                switch (symport.direction) {
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
                }

                pins.push_back(pin.id);
                buffer.pins.emplace(pin.id, std::move(pin));
                // 最后再收集connectPins
            }
            node.pins = pins;
            buffer.nodes.emplace(node.id, std::move(node));
        }
        else if (auto* prim = member.as_if<slang::ast::PrimitiveInstanceSymbol>()) {
            GraphicNode node;
            node.id = node_id++;

            // 1. 获取原语名称 (如果代码写的是 and u1(...)，名字就是 u1；如果是 and(...)，名字可能为空)
            node.name = prim->name.empty() ? "primitive" : std::string(prim->name);

            // 2. 获取原语类型 (例如 "and", "xor", "buf")
            node.typeName = "Gate";
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

            //const slang::ast::Symbol& symShell = prim->tostring;
            //node.typeName = std::string(prim->primitiveType.name);

            node.sourceLocation = GetSourceLocation(*prim, sm);

            // 3. 收集引脚
            std::vector<PinId> pins;
            auto connections = prim->getPortConnections();

            for (size_t i = 0; i < connections.size(); ++i) {
                auto* conn = connections[i];
                GraphicPin pin;
                pin.id = pin_id++;

                if (i == 0) {
                    pin.name = "out";
                    pin.direction = PinDirection::Out;
                }
                else {
                    pin.name = "in" + std::to_string(i);
                    pin.direction = PinDirection::In;
                }

                // 处理连接逻辑 (和 Instance 一样，后续需要通过 expr 找 Net)
                //if (auto* expr = conn->getExpression()) {
                //    // 这里逻辑和之前一致，用来存 connectedPins
                //}

                pins.push_back(pin.id);
                buffer.pins.emplace(pin.id, std::move(pin));
            }

            node.pins = pins;
            buffer.nodes.emplace(node.id, std::move(node));
        }
        //else if (auto* prim = member.as_if<slang::ast::NetSymbol>()) {

        //}
        //else if (auto* prim = member.as_if<slang::ast::ContinuousAssignSymbol>()) {
        //    GraphicNode node;
        //    node.id = node_id++;
        //    node.name = "";
        //    node.typeName = prim->kind

        //    const auto* syntax = prim->getSyntax();

        //}
    }
}





LogicView LogicFromSymbol(const slang::ast::Symbol& sym, const slang::SourceManager& sm) {
    LogicView view;

    // 1. 获取原始代码片段 (verilogCode)
    // 注意：Symbol 接口中只有 location，没有 getSourceRange()。
    // 我们需要通过 getSyntax() 获取语法节点，再拿到 Range。
    const auto* syntax = sym.getSyntax();
    if (syntax) {
        auto range = syntax->sourceRange();
        if (range != slang::SourceRange::NoLocation) {
            slang::SourceLocation start = range.start();
            slang::SourceLocation end = range.end();

            if (sm.isFileLoc(start) && sm.isFileLoc(end)) {
                slang::BufferID bid = start.buffer();
                uint32_t startOff = start.offset();
                uint32_t endOff = end.offset();
                // 获取该 Buffer 的全部文本，然后进行切片
                std::string_view fullText = sm.getSourceText(bid);
                if (endOff >= startOff && endOff <= fullText.size()) {
                    view.verilogCode = std::string(fullText.substr(startOff, endOff - startOff));
                }
            }
        }
    }

    //// 2. 处理门级原语 (PrimitiveInstanceSymbol)
    //if (sym.kind == slang::ast::SymbolKind::PrimitiveInstance) {
    //    // 使用 Symbol 接口中提供的 as<T> 进行强制转换
    //    const auto& prim = sym.as<slang::ast::PrimitiveInstanceSymbol>();
    //    // prim.primitiveType 是一个 PrimitiveSymbol，它有自己的 name
    //    view.expression = std::string(prim.primitiveType.name) + " gate logic";
    //}

    //// 3. 处理连续赋值语句 (ContinuousAssignSymbol)
    //else if (sym.kind == slang::ast::SymbolKind::ContinuousAssign) {
    //    const auto& asgn = sym.as<slang::ast::ContinuousAssignSymbol>();

    //    // getAssignment() 返回 Assignment 对象，该对象代表 "LHS = RHS"
    //    // format() 会将其渲染为标准的 Verilog 字符串
    //    view.expression = asgn.getAssignment().format();
    //}

    //// 4. 处理模块实例或 UDP 实例 (InstanceSymbol)
    //else if (sym.kind == slang::ast::SymbolKind::Instance) {
    //    const auto& inst = sym.as<slang::ast::InstanceSymbol>();

    //    // getDefinition() 返回该实例引用的定义 (DefinitionSymbol)
    //    const auto& def = inst.getDefinition();

    //    // 通过定义关联的语法节点判定其性质
    //    auto defSyntax = def.getSyntax();
    //    if (defSyntax && defSyntax->kind == slang::syntax::SyntaxKind::UdpDeclaration) {
    //        // 如果是 UDP，通常需要处理其真值表
    //        view.truthTable = ExtractTruthTable(def);
    //        view.expression = "UDP: " + std::string(def.name);
    //    }
    //    else {
    //        // 普通模块实例
    //        view.expression = "Module: " + std::string(def.name);
    //    }
    //}

    return view;
}



inline const char* ToString(slang::ast::SymbolKind kind) {
    using SK = slang::ast::SymbolKind;
    switch (kind) {
    case SK::Unknown: return "Unknown";
    case SK::Root: return "Root";
    case SK::Definition: return "Definition";
    case SK::CompilationUnit: return "CompilationUnit";
    case SK::DeferredMember: return "DeferredMember";
    case SK::TransparentMember: return "TransparentMember";
    case SK::EmptyMember: return "EmptyMember";
    case SK::PredefinedIntegerType: return "PredefinedIntegerType";
    case SK::ScalarType: return "ScalarType";
    case SK::FloatingType: return "FloatingType";
    case SK::EnumType: return "EnumType";
    case SK::EnumValue: return "EnumValue";
    case SK::PackedArrayType: return "PackedArrayType";
    case SK::FixedSizeUnpackedArrayType: return "FixedSizeUnpackedArrayType";
    case SK::DynamicArrayType: return "DynamicArrayType";
    case SK::DPIOpenArrayType: return "DPIOpenArrayType";
    case SK::AssociativeArrayType: return "AssociativeArrayType";
    case SK::QueueType: return "QueueType";
    case SK::PackedStructType: return "PackedStructType";
    case SK::UnpackedStructType: return "UnpackedStructType";
    case SK::PackedUnionType: return "PackedUnionType";
    case SK::UnpackedUnionType: return "UnpackedUnionType";
    case SK::ClassType: return "ClassType";
    case SK::CovergroupType: return "CovergroupType";
    case SK::VoidType: return "VoidType";
    case SK::NullType: return "NullType";
    case SK::CHandleType: return "CHandleType";
    case SK::StringType: return "StringType";
    case SK::EventType: return "EventType";
    case SK::UnboundedType: return "UnboundedType";
    case SK::TypeRefType: return "TypeRefType";
    case SK::UntypedType: return "UntypedType";
    case SK::SequenceType: return "SequenceType";
    case SK::PropertyType: return "PropertyType";
    case SK::VirtualInterfaceType: return "VirtualInterfaceType";
    case SK::TypeAlias: return "TypeAlias";
    case SK::ErrorType: return "ErrorType";
    case SK::ForwardingTypedef: return "ForwardingTypedef";
    case SK::NetType: return "NetType";
    case SK::Parameter: return "Parameter";
    case SK::TypeParameter: return "TypeParameter";
    case SK::Port: return "Port";
    case SK::MultiPort: return "MultiPort";
    case SK::InterfacePort: return "InterfacePort";
    case SK::Modport: return "Modport";
    case SK::ModportPort: return "ModportPort";
    case SK::ModportClocking: return "ModportClocking";
    case SK::Instance: return "Instance";
    case SK::InstanceBody: return "InstanceBody";
    case SK::InstanceArray: return "InstanceArray";
    case SK::Package: return "Package";
    case SK::ExplicitImport: return "ExplicitImport";
    case SK::WildcardImport: return "WildcardImport";
    case SK::Attribute: return "Attribute";
    case SK::Genvar: return "Genvar";
    case SK::GenerateBlock: return "GenerateBlock";
    case SK::GenerateBlockArray: return "GenerateBlockArray";
    case SK::ProceduralBlock: return "ProceduralBlock";
    case SK::StatementBlock: return "StatementBlock";
    case SK::Net: return "Net";
    case SK::Variable: return "Variable";
    case SK::FormalArgument: return "FormalArgument";
    case SK::Field: return "Field";
    case SK::ClassProperty: return "ClassProperty";
    case SK::Subroutine: return "Subroutine";
    case SK::ContinuousAssign: return "ContinuousAssign";
    case SK::ElabSystemTask: return "ElabSystemTask";
    case SK::GenericClassDef: return "GenericClassDef";
    case SK::MethodPrototype: return "MethodPrototype";
    case SK::UninstantiatedDef: return "UninstantiatedDef";
    case SK::Iterator: return "Iterator";
    case SK::PatternVar: return "PatternVar";
    case SK::ConstraintBlock: return "ConstraintBlock";
    case SK::DefParam: return "DefParam";
    case SK::Specparam: return "Specparam";
    case SK::Primitive: return "Primitive";
    case SK::PrimitivePort: return "PrimitivePort";
    case SK::PrimitiveInstance: return "PrimitiveInstance";
    case SK::SpecifyBlock: return "SpecifyBlock";
    case SK::Sequence: return "Sequence";
    case SK::Property: return "Property";
    case SK::AssertionPort: return "AssertionPort";
    case SK::ClockingBlock: return "ClockingBlock";
    case SK::ClockVar: return "ClockVar";
    case SK::LocalAssertionVar: return "LocalAssertionVar";
    case SK::LetDecl: return "LetDecl";
    case SK::Checker: return "Checker";
    case SK::CheckerInstance: return "CheckerInstance";
    case SK::CheckerInstanceBody: return "CheckerInstanceBody";
    case SK::RandSeqProduction: return "RandSeqProduction";
    case SK::CovergroupBody: return "CovergroupBody";
    case SK::Coverpoint: return "Coverpoint";
    case SK::CoverCross: return "CoverCross";
    case SK::CoverCrossBody: return "CoverCrossBody";
    case SK::CoverageBin: return "CoverageBin";
    case SK::TimingPath: return "TimingPath";
    case SK::PulseStyle: return "PulseStyle";
    case SK::SystemTimingCheck: return "SystemTimingCheck";
    case SK::AnonymousProgram: return "AnonymousProgram";
    case SK::NetAlias: return "NetAlias";
    case SK::ConfigBlock: return "ConfigBlock";
    default: return "Unknown";
    }
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
    for (NodeId topId : ss.topLevelModules) {
        auto it = ss.nodes.find(topId);
        if (it != ss.nodes.end()) {
            const auto& node = it->second;
            output += std::format("  [{}] {} ({}) {} [{}:{}]-[{}:{}]\n",
                node.id,
                node.name,
                node.typeName,
                node.sourceLocation.filePath,
                node.sourceLocation.startLine,
                node.sourceLocation.startCol,
                node.sourceLocation.endLine,
                node.sourceLocation.endCol);
        }
    }

    // 打印所有节点
    output += "\nAll nodes:\n";
    for (const auto& [id, node] : ss.nodes) {
        output += std::format("Node [{}] Name: {}, Type: {}\n", node.id, node.name, node.typeName);

        // 输出逻辑描述
        if (!node.logicDescription.verilogCode.empty())
            output += std::format("  Verilog code: {}\n", node.logicDescription.verilogCode);
        if (!node.logicDescription.expression.empty())
            output += std::format("  Expression: {}\n", node.logicDescription.expression);
        if (!node.logicDescription.truthTable.empty())
            output += std::format("  Truth Table: {}\n", node.logicDescription.truthTable);

        // 输出 pins
        output += "  Pins:\n";
        for (PinId pid : node.pins) {
            auto pit = ss.pins.find(pid);
            if (pit == ss.pins.end()) continue;
            const auto& pin = pit->second;

            output += std::format("    [{}] {} ({})", pin.id, pin.name,
                pin.direction == PinDirection::In ? "In" :
                pin.direction == PinDirection::Out ? "Out" :
                pin.direction == PinDirection::InOut ? "InOut" : "Ref");

            if (!pin.connectedPins.empty()) {
                output += " -> Connected to pins: ";
                for (PinId cid : pin.connectedPins) {
                    output += std::to_string(cid) + " ";
                }
            }
            output += "\n";
        }
        output += std::format("{} [{}:{}]-[{}:{}]\n", node.sourceLocation.filePath,
            node.sourceLocation.startLine,
            node.sourceLocation.startCol,
            node.sourceLocation.endLine,
            node.sourceLocation.endCol);

        output += "\n";
    }

    wxLogDebug("%s", output.c_str());
}

