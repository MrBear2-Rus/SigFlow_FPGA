
    void handle(const AcceptOnPropertyExprSyntax& node) {
        writeToken("keyword", node.keyword);
        writeToken("openParen", node.openParen);
        writeNode("condition", node.condition);
        writeToken("closeParen", node.closeParen);
        writeNode("expr", node.expr);
    }
    

    void handle(const ActionBlockSyntax& node) {
        writeOptionalNode("statement", node.statement);
        writeOptionalNode("elseClause", node.elseClause);
    }
    

    void handle(const AnonymousProgramSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeToken("semi", node.semi);
        writeSyntaxList("members", node.members);
        writeToken("endkeyword", node.endkeyword);
    }
    

    void handle(const AnsiPortListSyntax& node) {
        writeToken("openParen", node.openParen);
        writeSeparatedSyntaxList("ports", node.ports);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const AnsiUdpPortListSyntax& node) {
        writeToken("openParen", node.openParen);
        writeSeparatedSyntaxList("ports", node.ports);
        writeToken("closeParen", node.closeParen);
        writeToken("semi", node.semi);
    }
    

    void handle(const ArgumentListSyntax& node) {
        writeToken("openParen", node.openParen);
        writeSeparatedSyntaxList("parameters", node.parameters);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const ArrayOrRandomizeMethodExpressionSyntax& node) {
        writeNode("method", node.method);
        writeToken("with", node.with);
        writeOptionalNode("args", node.args);
        writeOptionalNode("constraints", node.constraints);
    }
    

    void handle(const AssertionItemPortListSyntax& node) {
        writeToken("openParen", node.openParen);
        writeSeparatedSyntaxList("ports", node.ports);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const AssertionItemPortSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("local", node.local);
        writeToken("direction", node.direction);
        writeNode("type", node.type);
        writeToken("name", node.name);
        writeSyntaxList("dimensions", node.dimensions);
        writeOptionalNode("defaultValue", node.defaultValue);
    }
    

    void handle(const AssignmentPatternExpressionSyntax& node) {
        writeOptionalNode("type", node.type);
        writeNode("pattern", node.pattern);
    }
    

    void handle(const AssignmentPatternItemSyntax& node) {
        writeNode("key", node.key);
        writeToken("colon", node.colon);
        writeNode("expr", node.expr);
    }
    

    void handle(const AttributeInstanceSyntax& node) {
        writeToken("openParen", node.openParen);
        writeToken("openStar", node.openStar);
        writeSeparatedSyntaxList("specs", node.specs);
        writeToken("closeStar", node.closeStar);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const AttributeSpecSyntax& node) {
        writeToken("name", node.name);
        writeOptionalNode("value", node.value);
    }
    

    void handle(const BadExpressionSyntax& node) {
        writeNode("expr", node.expr);
    }
    

    void handle(const BeginKeywordsDirectiveSyntax& node) {
        writeToken("directive", node.directive);
        writeToken("versionSpecifier", node.versionSpecifier);
    }
    

    void handle(const BinSelectWithFilterExprSyntax& node) {
        writeNode("expr", node.expr);
        writeToken("with", node.with);
        writeToken("openParen", node.openParen);
        writeNode("filter", node.filter);
        writeToken("closeParen", node.closeParen);
        writeOptionalNode("matchesClause", node.matchesClause);
    }
    

    void handle(const BinaryBinsSelectExprSyntax& node) {
        writeNode("left", node.left);
        writeToken("op", node.op);
        writeNode("right", node.right);
    }
    

    void handle(const BinaryBlockEventExpressionSyntax& node) {
        writeNode("left", node.left);
        writeToken("orKeyword", node.orKeyword);
        writeNode("right", node.right);
    }
    

    void handle(const BinaryConditionalDirectiveExpressionSyntax& node) {
        writeNode("left", node.left);
        writeToken("op", node.op);
        writeNode("right", node.right);
    }
    

    void handle(const BinaryEventExpressionSyntax& node) {
        writeNode("left", node.left);
        writeToken("operatorToken", node.operatorToken);
        writeNode("right", node.right);
    }
    

    void handle(const BinaryExpressionSyntax& node) {
        writeNode("left", node.left);
        writeToken("operatorToken", node.operatorToken);
        writeSyntaxList("attributes", node.attributes);
        writeNode("right", node.right);
    }
    

    void handle(const BinaryPropertyExprSyntax& node) {
        writeNode("left", node.left);
        writeToken("op", node.op);
        writeNode("right", node.right);
    }
    

    void handle(const BinarySequenceExprSyntax& node) {
        writeNode("left", node.left);
        writeToken("op", node.op);
        writeNode("right", node.right);
    }
    

    void handle(const BindDirectiveSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("bind", node.bind);
        writeNode("target", node.target);
        writeOptionalNode("targetInstances", node.targetInstances);
        writeNode("instantiation", node.instantiation);
    }
    

    void handle(const BindTargetListSyntax& node) {
        writeToken("colon", node.colon);
        writeSeparatedSyntaxList("targets", node.targets);
    }
    

    void handle(const BinsSelectConditionExprSyntax& node) {
        writeToken("binsof", node.binsof);
        writeToken("openParen", node.openParen);
        writeNode("name", node.name);
        writeToken("closeParen", node.closeParen);
        writeOptionalNode("intersects", node.intersects);
    }
    

    void handle(const BinsSelectionSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeToken("name", node.name);
        writeToken("equals", node.equals);
        writeNode("expr", node.expr);
        writeOptionalNode("iff", node.iff);
        writeToken("semi", node.semi);
    }
    

    void handle(const BitSelectSyntax& node) {
        writeNode("expr", node.expr);
    }
    

    void handle(const BlockCoverageEventSyntax& node) {
        writeToken("atat", node.atat);
        writeToken("openParen", node.openParen);
        writeNode("expr", node.expr);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const BlockStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeToken("begin", node.begin);
        writeOptionalNode("blockName", node.blockName);
        writeSyntaxList("items", node.items);
        writeToken("end", node.end);
        writeOptionalNode("endBlockName", node.endBlockName);
    }
    

    void handle(const CaseGenerateSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeToken("openParen", node.openParen);
        writeNode("condition", node.condition);
        writeToken("closeParen", node.closeParen);
        writeSyntaxList("items", node.items);
        writeToken("endCase", node.endCase);
    }
    

    void handle(const CasePropertyExprSyntax& node) {
        writeToken("caseKeyword", node.caseKeyword);
        writeToken("openParen", node.openParen);
        writeNode("expr", node.expr);
        writeToken("closeParen", node.closeParen);
        writeSyntaxList("items", node.items);
        writeToken("endcase", node.endcase);
    }
    

    void handle(const CaseStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeToken("uniqueOrPriority", node.uniqueOrPriority);
        writeToken("caseKeyword", node.caseKeyword);
        writeToken("openParen", node.openParen);
        writeNode("expr", node.expr);
        writeToken("closeParen", node.closeParen);
        writeToken("matchesOrInside", node.matchesOrInside);
        writeSyntaxList("items", node.items);
        writeToken("endcase", node.endcase);
    }
    

    void handle(const CastExpressionSyntax& node) {
        writeNode("left", node.left);
        writeToken("apostrophe", node.apostrophe);
        writeNode("right", node.right);
    }
    

    void handle(const CellConfigRuleSyntax& node) {
        writeToken("cell", node.cell);
        writeNode("name", node.name);
        writeNode("ruleClause", node.ruleClause);
        writeToken("semi", node.semi);
    }
    

    void handle(const ChargeStrengthSyntax& node) {
        writeToken("openParen", node.openParen);
        writeToken("strength", node.strength);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const CheckerDataDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("rand", node.rand);
        writeNode("data", node.data);
    }
    

    void handle(const CheckerDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeToken("name", node.name);
        writeOptionalNode("portList", node.portList);
        writeToken("semi", node.semi);
        writeSyntaxList("members", node.members);
        writeToken("end", node.end);
        writeOptionalNode("endBlockName", node.endBlockName);
    }
    

    void handle(const CheckerInstanceStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeNode("instance", node.instance);
    }
    

    void handle(const CheckerInstantiationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeNode("type", node.type);
        writeOptionalNode("parameters", node.parameters);
        writeSeparatedSyntaxList("instances", node.instances);
        writeToken("semi", node.semi);
    }
    

    void handle(const ClassDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("virtualOrInterface", node.virtualOrInterface);
        writeToken("classKeyword", node.classKeyword);
        writeOptionalNode("finalSpecifier", node.finalSpecifier);
        writeToken("name", node.name);
        writeOptionalNode("parameters", node.parameters);
        writeOptionalNode("extendsClause", node.extendsClause);
        writeOptionalNode("implementsClause", node.implementsClause);
        writeToken("semi", node.semi);
        writeSyntaxList("items", node.items);
        writeToken("endClass", node.endClass);
        writeOptionalNode("endBlockName", node.endBlockName);
    }
    

    void handle(const ClassMethodDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeTokenList("qualifiers", node.qualifiers);
        writeNode("declaration", node.declaration);
    }
    

    void handle(const ClassMethodPrototypeSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeTokenList("qualifiers", node.qualifiers);
        writeNode("prototype", node.prototype);
        writeToken("semi", node.semi);
    }
    

    void handle(const ClassNameSyntax& node) {
        writeToken("identifier", node.identifier);
        writeNode("parameters", node.parameters);
    }
    

    void handle(const ClassPropertyDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeTokenList("qualifiers", node.qualifiers);
        writeNode("declaration", node.declaration);
    }
    

    void handle(const ClassSpecifierSyntax& node) {
        writeToken("colon", node.colon);
        writeToken("keyword", node.keyword);
    }
    

    void handle(const ClockingDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("globalOrDefault", node.globalOrDefault);
        writeToken("clocking", node.clocking);
        writeToken("blockName", node.blockName);
        writeToken("at", node.at);
        writeNode("event", node.event);
        writeToken("semi", node.semi);
        writeSyntaxList("items", node.items);
        writeToken("endClocking", node.endClocking);
        writeOptionalNode("endBlockName", node.endBlockName);
    }
    

    void handle(const ClockingDirectionSyntax& node) {
        writeToken("input", node.input);
        writeOptionalNode("inputSkew", node.inputSkew);
        writeToken("output", node.output);
        writeOptionalNode("outputSkew", node.outputSkew);
    }
    

    void handle(const ClockingItemSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeNode("direction", node.direction);
        writeSeparatedSyntaxList("decls", node.decls);
        writeToken("semi", node.semi);
    }
    

    void handle(const ClockingPropertyExprSyntax& node) {
        writeNode("event", node.event);
        writeOptionalNode("expr", node.expr);
    }
    

    void handle(const ClockingSequenceExprSyntax& node) {
        writeNode("event", node.event);
        writeNode("expr", node.expr);
    }
    

    void handle(const ClockingSkewSyntax& node) {
        writeToken("edge", node.edge);
        writeOptionalNode("delay", node.delay);
    }
    

    void handle(const ColonExpressionClauseSyntax& node) {
        writeToken("colon", node.colon);
        writeNode("expr", node.expr);
    }
    

    void handle(const CompilationUnitSyntax& node) {
        writeSyntaxList("members", node.members);
        writeToken("endOfFile", node.endOfFile);
    }
    

    void handle(const ConcatenationExpressionSyntax& node) {
        writeToken("openBrace", node.openBrace);
        writeSeparatedSyntaxList("expressions", node.expressions);
        writeToken("closeBrace", node.closeBrace);
    }
    

    void handle(const ConcurrentAssertionMemberSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeNode("statement", node.statement);
    }
    

    void handle(const ConcurrentAssertionStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeToken("propertyOrSequence", node.propertyOrSequence);
        writeToken("openParen", node.openParen);
        writeNode("propertySpec", node.propertySpec);
        writeToken("closeParen", node.closeParen);
        writeNode("action", node.action);
    }
    

    void handle(const ConditionalBranchDirectiveSyntax& node) {
        writeToken("directive", node.directive);
        writeNode("expr", node.expr);
        writeTokenList("disabledTokens", node.disabledTokens);
    }
    

    void handle(const ConditionalConstraintSyntax& node) {
        writeToken("ifKeyword", node.ifKeyword);
        writeToken("openParen", node.openParen);
        writeNode("condition", node.condition);
        writeToken("closeParen", node.closeParen);
        writeNode("constraints", node.constraints);
        writeOptionalNode("elseClause", node.elseClause);
    }
    

    void handle(const ConditionalExpressionSyntax& node) {
        writeNode("predicate", node.predicate);
        writeToken("question", node.question);
        writeSyntaxList("attributes", node.attributes);
        writeNode("left", node.left);
        writeToken("colon", node.colon);
        writeNode("right", node.right);
    }
    

    void handle(const ConditionalPathDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeToken("openParen", node.openParen);
        writeNode("predicate", node.predicate);
        writeToken("closeParen", node.closeParen);
        writeNode("path", node.path);
    }
    

    void handle(const ConditionalPatternSyntax& node) {
        writeNode("expr", node.expr);
        writeOptionalNode("matchesClause", node.matchesClause);
    }
    

    void handle(const ConditionalPredicateSyntax& node) {
        writeSeparatedSyntaxList("conditions", node.conditions);
    }
    

    void handle(const ConditionalPropertyExprSyntax& node) {
        writeToken("ifKeyword", node.ifKeyword);
        writeToken("openParen", node.openParen);
        writeNode("condition", node.condition);
        writeToken("closeParen", node.closeParen);
        writeNode("expr", node.expr);
        writeOptionalNode("elseClause", node.elseClause);
    }
    

    void handle(const ConditionalStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeToken("uniqueOrPriority", node.uniqueOrPriority);
        writeToken("ifKeyword", node.ifKeyword);
        writeToken("openParen", node.openParen);
        writeNode("predicate", node.predicate);
        writeToken("closeParen", node.closeParen);
        writeNode("statement", node.statement);
        writeOptionalNode("elseClause", node.elseClause);
    }
    

    void handle(const ConfigCellIdentifierSyntax& node) {
        writeToken("library", node.library);
        writeToken("dot", node.dot);
        writeToken("cell", node.cell);
    }
    

    void handle(const ConfigDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("config", node.config);
        writeToken("name", node.name);
        writeToken("semi1", node.semi1);
        writeSyntaxList("localparams", node.localparams);
        writeToken("design", node.design);
        writeSyntaxList("topCells", node.topCells);
        writeToken("semi2", node.semi2);
        writeSyntaxList("rules", node.rules);
        writeToken("endconfig", node.endconfig);
        writeOptionalNode("blockName", node.blockName);
    }
    

    void handle(const ConfigInstanceIdentifierSyntax& node) {
        writeToken("dot", node.dot);
        writeToken("name", node.name);
    }
    

    void handle(const ConfigLiblistSyntax& node) {
        writeToken("liblist", node.liblist);
        writeTokenList("libraries", node.libraries);
    }
    

    void handle(const ConfigUseClauseSyntax& node) {
        writeToken("use", node.use);
        writeOptionalNode("name", node.name);
        writeOptionalNode("paramAssignments", node.paramAssignments);
        writeToken("colon", node.colon);
        writeToken("config", node.config);
    }
    

    void handle(const ConstraintBlockSyntax& node) {
        writeToken("openBrace", node.openBrace);
        writeSyntaxList("items", node.items);
        writeToken("closeBrace", node.closeBrace);
    }
    

    void handle(const ConstraintDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeTokenList("qualifiers", node.qualifiers);
        writeToken("keyword", node.keyword);
        writeSyntaxList("specifiers", node.specifiers);
        writeNode("name", node.name);
        writeNode("block", node.block);
    }
    

    void handle(const ConstraintPrototypeSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeTokenList("qualifiers", node.qualifiers);
        writeToken("keyword", node.keyword);
        writeSyntaxList("specifiers", node.specifiers);
        writeNode("name", node.name);
        writeToken("semi", node.semi);
    }
    

    void handle(const ContinuousAssignSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("assign", node.assign);
        writeOptionalNode("strength", node.strength);
        writeOptionalNode("delay", node.delay);
        writeSeparatedSyntaxList("assignments", node.assignments);
        writeToken("semi", node.semi);
    }
    

    void handle(const CopyClassExpressionSyntax& node) {
        writeNode("scopedNew", node.scopedNew);
        writeNode("expr", node.expr);
    }
    

    void handle(const CoverCrossSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeOptionalNode("label", node.label);
        writeToken("cross", node.cross);
        writeSeparatedSyntaxList("items", node.items);
        writeOptionalNode("iff", node.iff);
        writeToken("openBrace", node.openBrace);
        writeSyntaxList("members", node.members);
        writeToken("closeBrace", node.closeBrace);
        writeToken("emptySemi", node.emptySemi);
    }
    

    void handle(const CoverageBinsArraySizeSyntax& node) {
        writeToken("openBracket", node.openBracket);
        writeOptionalNode("expr", node.expr);
        writeToken("closeBracket", node.closeBracket);
    }
    

    void handle(const CoverageBinsSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("wildcard", node.wildcard);
        writeToken("keyword", node.keyword);
        writeToken("name", node.name);
        writeOptionalNode("size", node.size);
        writeToken("equals", node.equals);
        writeNode("initializer", node.initializer);
        writeOptionalNode("iff", node.iff);
        writeToken("semi", node.semi);
    }
    

    void handle(const CoverageIffClauseSyntax& node) {
        writeToken("iff", node.iff);
        writeToken("openParen", node.openParen);
        writeNode("expr", node.expr);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const CoverageOptionSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeNode("expr", node.expr);
        writeToken("semi", node.semi);
    }
    

    void handle(const CovergroupDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("covergroup", node.covergroup);
        writeToken("extends", node.extends);
        writeToken("name", node.name);
        writeOptionalNode("portList", node.portList);
        writeOptionalNode("event", node.event);
        writeToken("semi", node.semi);
        writeSyntaxList("members", node.members);
        writeToken("endgroup", node.endgroup);
        writeOptionalNode("endBlockName", node.endBlockName);
    }
    

    void handle(const CoverpointSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeNode("type", node.type);
        writeOptionalNode("label", node.label);
        writeToken("coverpoint", node.coverpoint);
        writeNode("expr", node.expr);
        writeOptionalNode("iff", node.iff);
        writeToken("openBrace", node.openBrace);
        writeSyntaxList("members", node.members);
        writeToken("closeBrace", node.closeBrace);
        writeToken("emptySemi", node.emptySemi);
    }
    

    void handle(const DPIExportSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeToken("specString", node.specString);
        writeToken("c_identifier", node.c_identifier);
        writeToken("equals", node.equals);
        writeToken("functionOrTask", node.functionOrTask);
        writeToken("name", node.name);
        writeToken("semi", node.semi);
    }
    

    void handle(const DPIImportSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeToken("specString", node.specString);
        writeToken("property", node.property);
        writeToken("c_identifier", node.c_identifier);
        writeToken("equals", node.equals);
        writeNode("method", node.method);
        writeToken("semi", node.semi);
    }
    

    void handle(const DataDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeTokenList("modifiers", node.modifiers);
        writeNode("type", node.type);
        writeSeparatedSyntaxList("declarators", node.declarators);
        writeToken("semi", node.semi);
    }
    

    void handle(const DeclaratorSyntax& node) {
        writeToken("name", node.name);
        writeSyntaxList("dimensions", node.dimensions);
        writeOptionalNode("initializer", node.initializer);
    }
    

    void handle(const DefParamAssignmentSyntax& node) {
        writeNode("name", node.name);
        writeNode("setter", node.setter);
    }
    

    void handle(const DefParamSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("defparam", node.defparam);
        writeSeparatedSyntaxList("assignments", node.assignments);
        writeToken("semi", node.semi);
    }
    

    void handle(const DefaultCaseItemSyntax& node) {
        writeToken("defaultKeyword", node.defaultKeyword);
        writeToken("colon", node.colon);
        writeNode("clause", node.clause);
    }
    

    void handle(const DefaultClockingReferenceSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("defaultKeyword", node.defaultKeyword);
        writeToken("clocking", node.clocking);
        writeToken("name", node.name);
        writeToken("semi", node.semi);
    }
    

    void handle(const DefaultConfigRuleSyntax& node) {
        writeToken("defaultKeyword", node.defaultKeyword);
        writeNode("liblist", node.liblist);
        writeToken("semi", node.semi);
    }
    

    void handle(const DefaultCoverageBinInitializerSyntax& node) {
        writeToken("defaultKeyword", node.defaultKeyword);
        writeToken("sequenceKeyword", node.sequenceKeyword);
    }
    

    void handle(const DefaultDecayTimeDirectiveSyntax& node) {
        writeToken("directive", node.directive);
        writeToken("time", node.time);
    }
    

    void handle(const DefaultDisableDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("defaultKeyword", node.defaultKeyword);
        writeToken("disableKeyword", node.disableKeyword);
        writeToken("iffKeyword", node.iffKeyword);
        writeNode("expr", node.expr);
        writeToken("semi", node.semi);
    }
    

    void handle(const DefaultDistItemSyntax& node) {
        writeToken("defaultKeyword", node.defaultKeyword);
        writeOptionalNode("weight", node.weight);
    }
    

    void handle(const DefaultExtendsClauseArgSyntax& node) {
        writeToken("openParen", node.openParen);
        writeToken("defaultKeyword", node.defaultKeyword);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const DefaultFunctionPortSyntax& node) {
        writeToken("keyword", node.keyword);
    }
    

    void handle(const DefaultNetTypeDirectiveSyntax& node) {
        writeToken("directive", node.directive);
        writeToken("netType", node.netType);
    }
    

    void handle(const DefaultPropertyCaseItemSyntax& node) {
        writeToken("defaultKeyword", node.defaultKeyword);
        writeToken("colon", node.colon);
        writeNode("expr", node.expr);
        writeToken("semi", node.semi);
    }
    

    void handle(const DefaultRsCaseItemSyntax& node) {
        writeToken("defaultKeyword", node.defaultKeyword);
        writeToken("colon", node.colon);
        writeNode("item", node.item);
        writeToken("semi", node.semi);
    }
    

    void handle(const DefaultSkewItemSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeNode("direction", node.direction);
        writeToken("semi", node.semi);
    }
    

    void handle(const DefaultTriregStrengthDirectiveSyntax& node) {
        writeToken("directive", node.directive);
        writeToken("strength", node.strength);
    }
    

    void handle(const DeferredAssertionSyntax& node) {
        writeToken("hash", node.hash);
        writeToken("zero", node.zero);
        writeToken("finalKeyword", node.finalKeyword);
    }
    

    void handle(const DefineDirectiveSyntax& node) {
        writeToken("directive", node.directive);
        writeToken("name", node.name);
        writeOptionalNode("formalArguments", node.formalArguments);
        writeTokenList("body", node.body);
    }
    

    void handle(const Delay3Syntax& node) {
        writeToken("hash", node.hash);
        writeToken("openParen", node.openParen);
        writeNode("delay1", node.delay1);
        writeToken("comma1", node.comma1);
        writeOptionalNode("delay2", node.delay2);
        writeToken("comma2", node.comma2);
        writeOptionalNode("delay3", node.delay3);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const DelaySyntax& node) {
        writeToken("hash", node.hash);
        writeNode("delayValue", node.delayValue);
    }
    

    void handle(const DelayedSequenceElementSyntax& node) {
        writeToken("doubleHash", node.doubleHash);
        writeOptionalNode("delayVal", node.delayVal);
        writeToken("openBracket", node.openBracket);
        writeToken("op", node.op);
        writeOptionalNode("range", node.range);
        writeToken("closeBracket", node.closeBracket);
        writeNode("expr", node.expr);
    }
    

    void handle(const DelayedSequenceExprSyntax& node) {
        writeOptionalNode("first", node.first);
        writeSyntaxList("elements", node.elements);
    }
    

    void handle(const DisableConstraintSyntax& node) {
        writeToken("disable", node.disable);
        writeToken("soft", node.soft);
        writeNode("name", node.name);
        writeToken("semi", node.semi);
    }
    

    void handle(const DisableForkStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeToken("disable", node.disable);
        writeToken("fork", node.fork);
        writeToken("semi", node.semi);
    }
    

    void handle(const DisableIffSyntax& node) {
        writeToken("disable", node.disable);
        writeToken("iff", node.iff);
        writeToken("openParen", node.openParen);
        writeNode("expr", node.expr);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const DisableStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeToken("disable", node.disable);
        writeNode("name", node.name);
        writeToken("semi", node.semi);
    }
    

    void handle(const DistConstraintListSyntax& node) {
        writeToken("dist", node.dist);
        writeToken("openBrace", node.openBrace);
        writeSeparatedSyntaxList("items", node.items);
        writeToken("closeBrace", node.closeBrace);
    }
    

    void handle(const DistItemSyntax& node) {
        writeNode("range", node.range);
        writeOptionalNode("weight", node.weight);
    }
    

    void handle(const DistWeightSyntax& node) {
        writeToken("op", node.op);
        writeToken("extraOp", node.extraOp);
        writeNode("expr", node.expr);
    }
    

    void handle(const DividerClauseSyntax& node) {
        writeToken("divide", node.divide);
        writeToken("value", node.value);
    }
    

    void handle(const DoWhileStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeToken("doKeyword", node.doKeyword);
        writeNode("statement", node.statement);
        writeToken("whileKeyword", node.whileKeyword);
        writeToken("openParen", node.openParen);
        writeNode("expr", node.expr);
        writeToken("closeParen", node.closeParen);
        writeToken("semi", node.semi);
    }
    

    void handle(const DotMemberClauseSyntax& node) {
        writeToken("dot", node.dot);
        writeToken("member", node.member);
    }
    

    void handle(const DriveStrengthSyntax& node) {
        writeToken("openParen", node.openParen);
        writeToken("strength0", node.strength0);
        writeToken("comma", node.comma);
        writeToken("strength1", node.strength1);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const EdgeControlSpecifierSyntax& node) {
        writeToken("openBracket", node.openBracket);
        writeSeparatedSyntaxList("descriptors", node.descriptors);
        writeToken("closeBracket", node.closeBracket);
    }
    

    void handle(const EdgeDescriptorSyntax& node) {
        writeToken("t1", node.t1);
        writeToken("t2", node.t2);
    }
    

    void handle(const EdgeSensitivePathSuffixSyntax& node) {
        writeToken("openParen", node.openParen);
        writeSeparatedSyntaxList("outputs", node.outputs);
        writeToken("polarityOperator", node.polarityOperator);
        writeToken("colon", node.colon);
        writeNode("expr", node.expr);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const ElabSystemTaskSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("name", node.name);
        writeOptionalNode("arguments", node.arguments);
        writeToken("semi", node.semi);
    }
    

    void handle(const ElementSelectExpressionSyntax& node) {
        writeNode("left", node.left);
        writeNode("select", node.select);
    }
    

    void handle(const ElementSelectSyntax& node) {
        writeToken("openBracket", node.openBracket);
        writeOptionalNode("selector", node.selector);
        writeToken("closeBracket", node.closeBracket);
    }
    

    void handle(const ElseClauseSyntax& node) {
        writeToken("elseKeyword", node.elseKeyword);
        writeNode("clause", node.clause);
    }
    

    void handle(const ElseConstraintClauseSyntax& node) {
        writeToken("elseKeyword", node.elseKeyword);
        writeNode("constraints", node.constraints);
    }
    

    void handle(const ElsePropertyClauseSyntax& node) {
        writeToken("elseKeyword", node.elseKeyword);
        writeNode("expr", node.expr);
    }
    

    void handle(const EmptyArgumentSyntax& node) {
        writeToken("placeholder", node.placeholder);
    }
    

    void handle(const EmptyIdentifierNameSyntax& node) {
        writeToken("placeholder", node.placeholder);
    }
    

    void handle(const EmptyMemberSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeTokenList("qualifiers", node.qualifiers);
        writeToken("semi", node.semi);
    }
    

    void handle(const EmptyNonAnsiPortSyntax& node) {
        writeToken("placeholder", node.placeholder);
    }
    

    void handle(const EmptyPortConnectionSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("placeholder", node.placeholder);
    }
    

    void handle(const EmptyQueueExpressionSyntax& node) {
        writeToken("openBrace", node.openBrace);
        writeToken("closeBrace", node.closeBrace);
    }
    

    void handle(const EmptyStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeToken("semicolon", node.semicolon);
    }
    

    void handle(const EmptyTimingCheckArgSyntax& node) {
        writeToken("placeholder", node.placeholder);
    }
    

    void handle(const EnumTypeSyntax& node) {
        writeToken("keyword", node.keyword);
        writeOptionalNode("baseType", node.baseType);
        writeToken("openBrace", node.openBrace);
        writeSeparatedSyntaxList("members", node.members);
        writeToken("closeBrace", node.closeBrace);
        writeSyntaxList("dimensions", node.dimensions);
    }
    

    void handle(const EqualsAssertionArgClauseSyntax& node) {
        writeToken("equals", node.equals);
        writeNode("expr", node.expr);
    }
    

    void handle(const EqualsTypeClauseSyntax& node) {
        writeToken("equals", node.equals);
        writeNode("type", node.type);
    }
    

    void handle(const EqualsValueClauseSyntax& node) {
        writeToken("equals", node.equals);
        writeNode("expr", node.expr);
    }
    

    void handle(const EventControlSyntax& node) {
        writeToken("at", node.at);
        writeNode("eventName", node.eventName);
    }
    

    void handle(const EventControlWithExpressionSyntax& node) {
        writeToken("at", node.at);
        writeNode("expr", node.expr);
    }
    

    void handle(const EventTriggerStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeToken("trigger", node.trigger);
        writeOptionalNode("timing", node.timing);
        writeNode("name", node.name);
        writeToken("semi", node.semi);
    }
    

    void handle(const ExplicitAnsiPortSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("direction", node.direction);
        writeToken("dot", node.dot);
        writeToken("name", node.name);
        writeToken("openParen", node.openParen);
        writeOptionalNode("expr", node.expr);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const ExplicitNonAnsiPortSyntax& node) {
        writeToken("dot", node.dot);
        writeToken("name", node.name);
        writeToken("openParen", node.openParen);
        writeOptionalNode("expr", node.expr);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const ExpressionConstraintSyntax& node) {
        writeToken("soft", node.soft);
        writeNode("expr", node.expr);
        writeToken("semi", node.semi);
    }
    

    void handle(const ExpressionCoverageBinInitializerSyntax& node) {
        writeNode("expr", node.expr);
    }
    

    void handle(const ExpressionOrDistSyntax& node) {
        writeNode("expr", node.expr);
        writeNode("distribution", node.distribution);
    }
    

    void handle(const ExpressionPatternSyntax& node) {
        writeNode("expr", node.expr);
    }
    

    void handle(const ExpressionStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeNode("expr", node.expr);
        writeToken("semi", node.semi);
    }
    

    void handle(const ExpressionTimingCheckArgSyntax& node) {
        writeNode("expr", node.expr);
    }
    

    void handle(const ExtendsClauseSyntax& node) {
        writeToken("keyword", node.keyword);
        writeNode("baseName", node.baseName);
        writeOptionalNode("arguments", node.arguments);
        writeOptionalNode("defaultedArg", node.defaultedArg);
    }
    

    void handle(const ExternInterfaceMethodSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("externKeyword", node.externKeyword);
        writeToken("forkJoin", node.forkJoin);
        writeNode("prototype", node.prototype);
        writeToken("semi", node.semi);
    }
    

    void handle(const ExternModuleDeclSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("externKeyword", node.externKeyword);
        writeSyntaxList("actualAttributes", node.actualAttributes);
        writeNode("header", node.header);
    }
    

    void handle(const ExternUdpDeclSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("externKeyword", node.externKeyword);
        writeSyntaxList("actualAttributes", node.actualAttributes);
        writeToken("primitive", node.primitive);
        writeToken("name", node.name);
        writeNode("portList", node.portList);
    }
    

    void handle(const FilePathSpecSyntax& node) {
        writeToken("path", node.path);
    }
    

    void handle(const FirstMatchSequenceExprSyntax& node) {
        writeToken("first_match", node.first_match);
        writeToken("openParen", node.openParen);
        writeNode("expr", node.expr);
        writeOptionalNode("matchList", node.matchList);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const ForLoopStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeToken("forKeyword", node.forKeyword);
        writeToken("openParen", node.openParen);
        writeSeparatedSyntaxList("initializers", node.initializers);
        writeToken("semi1", node.semi1);
        writeOptionalNode("stopExpr", node.stopExpr);
        writeToken("semi2", node.semi2);
        writeSeparatedSyntaxList("steps", node.steps);
        writeToken("closeParen", node.closeParen);
        writeNode("statement", node.statement);
    }
    

    void handle(const ForVariableDeclarationSyntax& node) {
        writeToken("varKeyword", node.varKeyword);
        writeOptionalNode("type", node.type);
        writeNode("declarator", node.declarator);
    }
    

    void handle(const ForeachLoopListSyntax& node) {
        writeToken("openParen", node.openParen);
        writeNode("arrayName", node.arrayName);
        writeToken("openBracket", node.openBracket);
        writeSeparatedSyntaxList("loopVariables", node.loopVariables);
        writeToken("closeBracket", node.closeBracket);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const ForeachLoopStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeNode("loopList", node.loopList);
        writeNode("statement", node.statement);
    }
    

    void handle(const ForeverStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeToken("foreverKeyword", node.foreverKeyword);
        writeNode("statement", node.statement);
    }
    

    void handle(const ForwardTypeRestrictionSyntax& node) {
        writeToken("keyword1", node.keyword1);
        writeToken("keyword2", node.keyword2);
    }
    

    void handle(const ForwardTypedefDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("typedefKeyword", node.typedefKeyword);
        writeOptionalNode("typeRestriction", node.typeRestriction);
        writeToken("name", node.name);
        writeToken("semi", node.semi);
    }
    

    void handle(const FunctionDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeNode("prototype", node.prototype);
        writeToken("semi", node.semi);
        writeSyntaxList("items", node.items);
        writeToken("end", node.end);
        writeOptionalNode("endBlockName", node.endBlockName);
    }
    

    void handle(const FunctionPortListSyntax& node) {
        writeToken("openParen", node.openParen);
        writeSeparatedSyntaxList("ports", node.ports);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const FunctionPortSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("constKeyword", node.constKeyword);
        writeToken("direction", node.direction);
        writeToken("staticKeyword", node.staticKeyword);
        writeToken("varKeyword", node.varKeyword);
        writeOptionalNode("dataType", node.dataType);
        writeNode("declarator", node.declarator);
    }
    

    void handle(const FunctionPrototypeSyntax& node) {
        writeToken("keyword", node.keyword);
        writeSyntaxList("specifiers", node.specifiers);
        writeToken("lifetime", node.lifetime);
        writeNode("returnType", node.returnType);
        writeNode("name", node.name);
        writeOptionalNode("portList", node.portList);
    }
    

    void handle(const GenerateBlockSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeOptionalNode("label", node.label);
        writeToken("begin", node.begin);
        writeOptionalNode("beginName", node.beginName);
        writeSyntaxList("members", node.members);
        writeToken("end", node.end);
        writeOptionalNode("endName", node.endName);
    }
    

    void handle(const GenerateRegionSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeSyntaxList("members", node.members);
        writeToken("endgenerate", node.endgenerate);
    }
    

    void handle(const GenvarDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeSeparatedSyntaxList("identifiers", node.identifiers);
        writeToken("semi", node.semi);
    }
    

    void handle(const HierarchicalInstanceSyntax& node) {
        writeOptionalNode("decl", node.decl);
        writeToken("openParen", node.openParen);
        writeSeparatedSyntaxList("connections", node.connections);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const HierarchyInstantiationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("type", node.type);
        writeOptionalNode("parameters", node.parameters);
        writeSeparatedSyntaxList("instances", node.instances);
        writeToken("semi", node.semi);
    }
    

    void handle(const IdWithExprCoverageBinInitializerSyntax& node) {
        writeToken("id", node.id);
        writeNode("withClause", node.withClause);
    }
    

    void handle(const IdentifierNameSyntax& node) {
        writeToken("identifier", node.identifier);
    }
    

    void handle(const IdentifierSelectNameSyntax& node) {
        writeToken("identifier", node.identifier);
        writeSyntaxList("selectors", node.selectors);
    }
    

    void handle(const IfGenerateSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeToken("openParen", node.openParen);
        writeNode("condition", node.condition);
        writeToken("closeParen", node.closeParen);
        writeNode("block", node.block);
        writeOptionalNode("elseClause", node.elseClause);
    }
    

    void handle(const IfNonePathDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeNode("path", node.path);
    }
    

    void handle(const IffEventClauseSyntax& node) {
        writeToken("iff", node.iff);
        writeNode("expr", node.expr);
    }
    

    void handle(const ImmediateAssertionMemberSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeNode("statement", node.statement);
    }
    

    void handle(const ImmediateAssertionStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeOptionalNode("delay", node.delay);
        writeNode("expr", node.expr);
        writeNode("action", node.action);
    }
    

    void handle(const ImplementsClauseSyntax& node) {
        writeToken("keyword", node.keyword);
        writeSeparatedSyntaxList("interfaces", node.interfaces);
    }
    

    void handle(const ImplicationConstraintSyntax& node) {
        writeNode("left", node.left);
        writeToken("arrow", node.arrow);
        writeNode("constraints", node.constraints);
    }
    

    void handle(const ImplicitAnsiPortSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeNode("header", node.header);
        writeNode("declarator", node.declarator);
    }
    

    void handle(const ImplicitEventControlSyntax& node) {
        writeToken("at", node.at);
        writeToken("openParen", node.openParen);
        writeToken("star", node.star);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const ImplicitNonAnsiPortSyntax& node) {
        writeNode("expr", node.expr);
    }
    

    void handle(const ImplicitTypeSyntax& node) {
        writeToken("signing", node.signing);
        writeSyntaxList("dimensions", node.dimensions);
        writeToken("placeholder", node.placeholder);
    }
    

    void handle(const IncludeDirectiveSyntax& node) {
        writeToken("directive", node.directive);
        writeToken("fileName", node.fileName);
    }
    

    void handle(const InsideExpressionSyntax& node) {
        writeNode("expr", node.expr);
        writeToken("inside", node.inside);
        writeNode("ranges", node.ranges);
    }
    

    void handle(const InstanceConfigRuleSyntax& node) {
        writeToken("instance", node.instance);
        writeToken("topModule", node.topModule);
        writeSyntaxList("instanceNames", node.instanceNames);
        writeNode("ruleClause", node.ruleClause);
        writeToken("semi", node.semi);
    }
    

    void handle(const InstanceNameSyntax& node) {
        writeToken("name", node.name);
        writeSyntaxList("dimensions", node.dimensions);
    }
    

    void handle(const IntegerTypeSyntax& node) {
        writeToken("keyword", node.keyword);
        writeToken("signing", node.signing);
        writeSyntaxList("dimensions", node.dimensions);
    }
    

    void handle(const IntegerVectorExpressionSyntax& node) {
        writeToken("size", node.size);
        writeToken("base", node.base);
        writeToken("value", node.value);
    }
    

    void handle(const InterfacePortHeaderSyntax& node) {
        writeToken("nameOrKeyword", node.nameOrKeyword);
        writeOptionalNode("modport", node.modport);
    }
    

    void handle(const IntersectClauseSyntax& node) {
        writeToken("intersect", node.intersect);
        writeNode("ranges", node.ranges);
    }
    

    void handle(const InvocationExpressionSyntax& node) {
        writeNode("left", node.left);
        writeSyntaxList("attributes", node.attributes);
        writeOptionalNode("arguments", node.arguments);
    }
    

    void handle(const JumpStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeToken("breakOrContinue", node.breakOrContinue);
        writeToken("semi", node.semi);
    }
    

    void handle(const KeywordNameSyntax& node) {
        writeToken("keyword", node.keyword);
    }
    

    void handle(const KeywordTypeSyntax& node) {
        writeToken("keyword", node.keyword);
    }
    

    void handle(const LetDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("let", node.let);
        writeToken("identifier", node.identifier);
        writeOptionalNode("portList", node.portList);
        writeToken("equals", node.equals);
        writeNode("expr", node.expr);
        writeToken("semi", node.semi);
    }
    

    void handle(const LibraryDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("library", node.library);
        writeToken("name", node.name);
        writeSeparatedSyntaxList("filePaths", node.filePaths);
        writeOptionalNode("incDirClause", node.incDirClause);
        writeToken("semi", node.semi);
    }
    

    void handle(const LibraryIncDirClauseSyntax& node) {
        writeToken("minus", node.minus);
        writeToken("incdir", node.incdir);
        writeSeparatedSyntaxList("filePaths", node.filePaths);
    }
    

    void handle(const LibraryIncludeStatementSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("include", node.include);
        writeNode("filePath", node.filePath);
        writeToken("semi", node.semi);
    }
    

    void handle(const LibraryMapSyntax& node) {
        writeSyntaxList("members", node.members);
        writeToken("endOfFile", node.endOfFile);
    }
    

    void handle(const LineDirectiveSyntax& node) {
        writeToken("directive", node.directive);
        writeToken("lineNumber", node.lineNumber);
        writeToken("fileName", node.fileName);
        writeToken("level", node.level);
    }
    

    void handle(const LiteralExpressionSyntax& node) {
        writeToken("literal", node.literal);
    }
    

    void handle(const LocalVariableDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("var", node.var);
        writeNode("type", node.type);
        writeSeparatedSyntaxList("declarators", node.declarators);
        writeToken("semi", node.semi);
    }
    

    void handle(const LoopConstraintSyntax& node) {
        writeToken("foreachKeyword", node.foreachKeyword);
        writeNode("loopList", node.loopList);
        writeNode("constraints", node.constraints);
    }
    

    void handle(const LoopGenerateSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeToken("openParen", node.openParen);
        writeToken("genvar", node.genvar);
        writeToken("identifier", node.identifier);
        writeToken("equals", node.equals);
        writeNode("initialExpr", node.initialExpr);
        writeToken("semi1", node.semi1);
        writeNode("stopExpr", node.stopExpr);
        writeToken("semi2", node.semi2);
        writeNode("iterationExpr", node.iterationExpr);
        writeToken("closeParen", node.closeParen);
        writeNode("block", node.block);
    }
    

    void handle(const LoopStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeToken("repeatOrWhile", node.repeatOrWhile);
        writeToken("openParen", node.openParen);
        writeNode("expr", node.expr);
        writeToken("closeParen", node.closeParen);
        writeNode("statement", node.statement);
    }
    

    void handle(const MacroActualArgumentListSyntax& node) {
        writeToken("openParen", node.openParen);
        writeSeparatedSyntaxList("args", node.args);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const MacroActualArgumentSyntax& node) {
        writeTokenList("tokens", node.tokens);
    }
    

    void handle(const MacroArgumentDefaultSyntax& node) {
        writeToken("equals", node.equals);
        writeTokenList("tokens", node.tokens);
    }
    

    void handle(const MacroFormalArgumentListSyntax& node) {
        writeToken("openParen", node.openParen);
        writeSeparatedSyntaxList("args", node.args);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const MacroFormalArgumentSyntax& node) {
        writeToken("name", node.name);
        writeOptionalNode("defaultValue", node.defaultValue);
    }
    

    void handle(const MacroUsageSyntax& node) {
        writeToken("directive", node.directive);
        writeOptionalNode("args", node.args);
    }
    

    void handle(const MatchesClauseSyntax& node) {
        writeToken("matchesKeyword", node.matchesKeyword);
        writeNode("pattern", node.pattern);
    }
    

    void handle(const MemberAccessExpressionSyntax& node) {
        writeNode("left", node.left);
        writeToken("dot", node.dot);
        writeToken("name", node.name);
    }
    

    void handle(const MinTypMaxExpressionSyntax& node) {
        writeNode("min", node.min);
        writeToken("colon1", node.colon1);
        writeNode("typ", node.typ);
        writeToken("colon2", node.colon2);
        writeNode("max", node.max);
    }
    

    void handle(const ModportClockingPortSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("clocking", node.clocking);
        writeToken("name", node.name);
    }
    

    void handle(const ModportDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeSeparatedSyntaxList("items", node.items);
        writeToken("semi", node.semi);
    }
    

    void handle(const ModportExplicitPortSyntax& node) {
        writeToken("dot", node.dot);
        writeToken("name", node.name);
        writeToken("openParen", node.openParen);
        writeOptionalNode("expr", node.expr);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const ModportItemSyntax& node) {
        writeToken("name", node.name);
        writeNode("ports", node.ports);
    }
    

    void handle(const ModportNamedPortSyntax& node) {
        writeToken("name", node.name);
    }
    

    void handle(const ModportSimplePortListSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("direction", node.direction);
        writeSeparatedSyntaxList("ports", node.ports);
    }
    

    void handle(const ModportSubroutinePortListSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("importExport", node.importExport);
        writeSeparatedSyntaxList("ports", node.ports);
    }
    

    void handle(const ModportSubroutinePortSyntax& node) {
        writeNode("prototype", node.prototype);
    }
    

    void handle(const ModuleDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeNode("header", node.header);
        writeSyntaxList("members", node.members);
        writeToken("endmodule", node.endmodule);
        writeOptionalNode("blockName", node.blockName);
    }
    

    void handle(const ModuleHeaderSyntax& node) {
        writeToken("moduleKeyword", node.moduleKeyword);
        writeToken("lifetime", node.lifetime);
        writeToken("name", node.name);
        writeSyntaxList("imports", node.imports);
        writeOptionalNode("parameters", node.parameters);
        writeOptionalNode("ports", node.ports);
        writeToken("semi", node.semi);
    }
    

    void handle(const MultipleConcatenationExpressionSyntax& node) {
        writeToken("openBrace", node.openBrace);
        writeNode("expression", node.expression);
        writeNode("concatenation", node.concatenation);
        writeToken("closeBrace", node.closeBrace);
    }
    

    void handle(const NameValuePragmaExpressionSyntax& node) {
        writeToken("name", node.name);
        writeToken("equals", node.equals);
        writeNode("value", node.value);
    }
    

    void handle(const NamedArgumentSyntax& node) {
        writeToken("dot", node.dot);
        writeToken("name", node.name);
        writeToken("openParen", node.openParen);
        writeOptionalNode("expr", node.expr);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const NamedBlockClauseSyntax& node) {
        writeToken("colon", node.colon);
        writeToken("name", node.name);
    }
    

    void handle(const NamedConditionalDirectiveExpressionSyntax& node) {
        writeToken("name", node.name);
    }
    

    void handle(const NamedLabelSyntax& node) {
        writeToken("name", node.name);
        writeToken("colon", node.colon);
    }
    

    void handle(const NamedParamAssignmentSyntax& node) {
        writeToken("dot", node.dot);
        writeToken("name", node.name);
        writeToken("openParen", node.openParen);
        writeOptionalNode("expr", node.expr);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const NamedPortConnectionSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("dot", node.dot);
        writeToken("name", node.name);
        writeToken("openParen", node.openParen);
        writeOptionalNode("expr", node.expr);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const NamedStructurePatternMemberSyntax& node) {
        writeToken("name", node.name);
        writeToken("colon", node.colon);
        writeNode("pattern", node.pattern);
    }
    

    void handle(const NamedTypeSyntax& node) {
        writeNode("name", node.name);
    }
    

    void handle(const NetAliasSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeSeparatedSyntaxList("nets", node.nets);
        writeToken("semi", node.semi);
    }
    

    void handle(const NetDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("netType", node.netType);
        writeOptionalNode("strength", node.strength);
        writeToken("expansionHint", node.expansionHint);
        writeNode("type", node.type);
        writeOptionalNode("delay", node.delay);
        writeSeparatedSyntaxList("declarators", node.declarators);
        writeToken("semi", node.semi);
    }
    

    void handle(const NetPortHeaderSyntax& node) {
        writeToken("direction", node.direction);
        writeToken("netType", node.netType);
        writeNode("dataType", node.dataType);
    }
    

    void handle(const NetTypeDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeNode("type", node.type);
        writeToken("name", node.name);
        writeOptionalNode("withFunction", node.withFunction);
        writeToken("semi", node.semi);
    }
    

    void handle(const NewArrayExpressionSyntax& node) {
        writeNode("newKeyword", node.newKeyword);
        writeToken("openBracket", node.openBracket);
        writeNode("sizeExpr", node.sizeExpr);
        writeToken("closeBracket", node.closeBracket);
        writeOptionalNode("initializer", node.initializer);
    }
    

    void handle(const NewClassExpressionSyntax& node) {
        writeNode("scopedNew", node.scopedNew);
        writeOptionalNode("argList", node.argList);
    }
    

    void handle(const NonAnsiPortListSyntax& node) {
        writeToken("openParen", node.openParen);
        writeSeparatedSyntaxList("ports", node.ports);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const NonAnsiUdpPortListSyntax& node) {
        writeToken("openParen", node.openParen);
        writeSeparatedSyntaxList("ports", node.ports);
        writeToken("closeParen", node.closeParen);
        writeToken("semi", node.semi);
    }
    

    void handle(const NumberPragmaExpressionSyntax& node) {
        writeToken("size", node.size);
        writeToken("base", node.base);
        writeToken("value", node.value);
    }
    

    void handle(const OneStepDelaySyntax& node) {
        writeToken("hash", node.hash);
        writeToken("oneStep", node.oneStep);
    }
    

    void handle(const OrderedArgumentSyntax& node) {
        writeNode("expr", node.expr);
    }
    

    void handle(const OrderedParamAssignmentSyntax& node) {
        writeNode("expr", node.expr);
    }
    

    void handle(const OrderedPortConnectionSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeNode("expr", node.expr);
    }
    

    void handle(const OrderedStructurePatternMemberSyntax& node) {
        writeNode("pattern", node.pattern);
    }
    

    void handle(const PackageExportAllDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeToken("star1", node.star1);
        writeToken("doubleColon", node.doubleColon);
        writeToken("star2", node.star2);
        writeToken("semi", node.semi);
    }
    

    void handle(const PackageExportDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeSeparatedSyntaxList("items", node.items);
        writeToken("semi", node.semi);
    }
    

    void handle(const PackageImportDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeSeparatedSyntaxList("items", node.items);
        writeToken("semi", node.semi);
    }
    

    void handle(const PackageImportItemSyntax& node) {
        writeToken("package", node.package);
        writeToken("doubleColon", node.doubleColon);
        writeToken("item", node.item);
    }
    

    void handle(const ParameterDeclarationStatementSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeNode("parameter", node.parameter);
        writeToken("semi", node.semi);
    }
    

    void handle(const ParameterDeclarationSyntax& node) {
        writeToken("keyword", node.keyword);
        writeNode("type", node.type);
        writeSeparatedSyntaxList("declarators", node.declarators);
    }
    

    void handle(const ParameterPortListSyntax& node) {
        writeToken("hash", node.hash);
        writeToken("openParen", node.openParen);
        writeSeparatedSyntaxList("declarations", node.declarations);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const ParameterValueAssignmentSyntax& node) {
        writeToken("hash", node.hash);
        writeToken("openParen", node.openParen);
        writeSeparatedSyntaxList("parameters", node.parameters);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const ParenExpressionListSyntax& node) {
        writeToken("openParen", node.openParen);
        writeSeparatedSyntaxList("expressions", node.expressions);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const ParenPragmaExpressionSyntax& node) {
        writeToken("openParen", node.openParen);
        writeSeparatedSyntaxList("values", node.values);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const ParenthesizedBinsSelectExprSyntax& node) {
        writeToken("openParen", node.openParen);
        writeNode("expr", node.expr);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const ParenthesizedConditionalDirectiveExpressionSyntax& node) {
        writeToken("openParen", node.openParen);
        writeNode("operand", node.operand);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const ParenthesizedEventExpressionSyntax& node) {
        writeToken("openParen", node.openParen);
        writeNode("expr", node.expr);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const ParenthesizedExpressionSyntax& node) {
        writeToken("openParen", node.openParen);
        writeNode("expression", node.expression);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const ParenthesizedPatternSyntax& node) {
        writeToken("openParen", node.openParen);
        writeNode("pattern", node.pattern);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const ParenthesizedPropertyExprSyntax& node) {
        writeToken("openParen", node.openParen);
        writeNode("expr", node.expr);
        writeOptionalNode("matchList", node.matchList);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const ParenthesizedSequenceExprSyntax& node) {
        writeToken("openParen", node.openParen);
        writeNode("expr", node.expr);
        writeOptionalNode("matchList", node.matchList);
        writeToken("closeParen", node.closeParen);
        writeOptionalNode("repetition", node.repetition);
    }
    

    void handle(const PathDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeNode("desc", node.desc);
        writeToken("equals", node.equals);
        writeToken("openParen", node.openParen);
        writeSeparatedSyntaxList("delays", node.delays);
        writeToken("closeParen", node.closeParen);
        writeToken("semi", node.semi);
    }
    

    void handle(const PathDescriptionSyntax& node) {
        writeToken("openParen", node.openParen);
        writeToken("edgeIdentifier", node.edgeIdentifier);
        writeSeparatedSyntaxList("inputs", node.inputs);
        writeToken("polarityOperator", node.polarityOperator);
        writeToken("pathOperator", node.pathOperator);
        writeNode("suffix", node.suffix);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const PatternCaseItemSyntax& node) {
        writeNode("pattern", node.pattern);
        writeToken("tripleAnd", node.tripleAnd);
        writeOptionalNode("expr", node.expr);
        writeToken("colon", node.colon);
        writeNode("statement", node.statement);
    }
    

    void handle(const PortConcatenationSyntax& node) {
        writeToken("openBrace", node.openBrace);
        writeSeparatedSyntaxList("references", node.references);
        writeToken("closeBrace", node.closeBrace);
    }
    

    void handle(const PortDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeNode("header", node.header);
        writeSeparatedSyntaxList("declarators", node.declarators);
        writeToken("semi", node.semi);
    }
    

    void handle(const PortReferenceSyntax& node) {
        writeToken("name", node.name);
        writeOptionalNode("select", node.select);
    }
    

    void handle(const PostfixUnaryExpressionSyntax& node) {
        writeNode("operand", node.operand);
        writeSyntaxList("attributes", node.attributes);
        writeToken("operatorToken", node.operatorToken);
    }
    

    void handle(const PragmaDirectiveSyntax& node) {
        writeToken("directive", node.directive);
        writeToken("name", node.name);
        writeSeparatedSyntaxList("args", node.args);
    }
    

    void handle(const PrefixUnaryExpressionSyntax& node) {
        writeToken("operatorToken", node.operatorToken);
        writeSyntaxList("attributes", node.attributes);
        writeNode("operand", node.operand);
    }
    

    void handle(const PrimaryBlockEventExpressionSyntax& node) {
        writeToken("keyword", node.keyword);
        writeNode("name", node.name);
    }
    

    void handle(const PrimitiveInstantiationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("type", node.type);
        writeOptionalNode("strength", node.strength);
        writeOptionalNode("delay", node.delay);
        writeSeparatedSyntaxList("instances", node.instances);
        writeToken("semi", node.semi);
    }
    

    void handle(const ProceduralAssignStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeNode("expr", node.expr);
        writeToken("semi", node.semi);
    }
    

    void handle(const ProceduralBlockSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeNode("statement", node.statement);
    }
    

    void handle(const ProceduralDeassignStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeNode("variable", node.variable);
        writeToken("semi", node.semi);
    }
    

    void handle(const ProductionSyntax& node) {
        writeOptionalNode("dataType", node.dataType);
        writeToken("name", node.name);
        writeOptionalNode("portList", node.portList);
        writeToken("colon", node.colon);
        writeSeparatedSyntaxList("rules", node.rules);
        writeToken("semi", node.semi);
    }
    

    void handle(const PropertyDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeToken("name", node.name);
        writeOptionalNode("portList", node.portList);
        writeToken("semi", node.semi);
        writeSyntaxList("variables", node.variables);
        writeNode("propertySpec", node.propertySpec);
        writeToken("optionalSemi", node.optionalSemi);
        writeToken("end", node.end);
        writeOptionalNode("endBlockName", node.endBlockName);
    }
    

    void handle(const PropertySpecSyntax& node) {
        writeOptionalNode("clocking", node.clocking);
        writeOptionalNode("disable", node.disable);
        writeNode("expr", node.expr);
    }
    

    void handle(const PullStrengthSyntax& node) {
        writeToken("openParen", node.openParen);
        writeToken("strength", node.strength);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const PulseStyleDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeSeparatedSyntaxList("inputs", node.inputs);
        writeToken("semi", node.semi);
    }
    

    void handle(const QueueDimensionSpecifierSyntax& node) {
        writeToken("dollar", node.dollar);
        writeOptionalNode("maxSizeClause", node.maxSizeClause);
    }
    

    void handle(const RandCaseItemSyntax& node) {
        writeNode("expr", node.expr);
        writeToken("colon", node.colon);
        writeNode("statement", node.statement);
    }
    

    void handle(const RandCaseStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeToken("randCase", node.randCase);
        writeSyntaxList("items", node.items);
        writeToken("endCase", node.endCase);
    }
    

    void handle(const RandJoinClauseSyntax& node) {
        writeToken("rand", node.rand);
        writeToken("join", node.join);
        writeOptionalNode("expr", node.expr);
    }
    

    void handle(const RandSequenceStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeToken("randsequence", node.randsequence);
        writeToken("openParen", node.openParen);
        writeToken("firstProduction", node.firstProduction);
        writeToken("closeParen", node.closeParen);
        writeSyntaxList("productions", node.productions);
        writeToken("endsequence", node.endsequence);
    }
    

    void handle(const RangeCoverageBinInitializerSyntax& node) {
        writeNode("ranges", node.ranges);
        writeOptionalNode("withClause", node.withClause);
    }
    

    void handle(const RangeDimensionSpecifierSyntax& node) {
        writeNode("selector", node.selector);
    }
    

    void handle(const RangeListSyntax& node) {
        writeToken("openBrace", node.openBrace);
        writeSeparatedSyntaxList("valueRanges", node.valueRanges);
        writeToken("closeBrace", node.closeBrace);
    }
    

    void handle(const RangeSelectSyntax& node) {
        writeNode("left", node.left);
        writeToken("range", node.range);
        writeNode("right", node.right);
    }
    

    void handle(const RepeatedEventControlSyntax& node) {
        writeToken("repeat", node.repeat);
        writeToken("openParen", node.openParen);
        writeNode("expr", node.expr);
        writeToken("closeParen", node.closeParen);
        writeOptionalNode("eventControl", node.eventControl);
    }
    

    void handle(const ReplicatedAssignmentPatternSyntax& node) {
        writeToken("openBrace", node.openBrace);
        writeNode("countExpr", node.countExpr);
        writeToken("innerOpenBrace", node.innerOpenBrace);
        writeSeparatedSyntaxList("items", node.items);
        writeToken("innerCloseBrace", node.innerCloseBrace);
        writeToken("closeBrace", node.closeBrace);
    }
    

    void handle(const ReturnStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeToken("returnKeyword", node.returnKeyword);
        writeOptionalNode("returnValue", node.returnValue);
        writeToken("semi", node.semi);
    }
    

    void handle(const RsCaseSyntax& node) {
        writeToken("keyword", node.keyword);
        writeToken("openParen", node.openParen);
        writeNode("expr", node.expr);
        writeToken("closeParen", node.closeParen);
        writeSyntaxList("items", node.items);
        writeToken("endcase", node.endcase);
    }
    

    void handle(const RsCodeBlockSyntax& node) {
        writeToken("openBrace", node.openBrace);
        writeSyntaxList("items", node.items);
        writeToken("closeBrace", node.closeBrace);
    }
    

    void handle(const RsElseClauseSyntax& node) {
        writeToken("keyword", node.keyword);
        writeNode("item", node.item);
    }
    

    void handle(const RsIfElseSyntax& node) {
        writeToken("keyword", node.keyword);
        writeToken("openParen", node.openParen);
        writeNode("condition", node.condition);
        writeToken("closeParen", node.closeParen);
        writeNode("ifItem", node.ifItem);
        writeOptionalNode("elseClause", node.elseClause);
    }
    

    void handle(const RsProdItemSyntax& node) {
        writeToken("name", node.name);
        writeOptionalNode("argList", node.argList);
    }
    

    void handle(const RsRepeatSyntax& node) {
        writeToken("keyword", node.keyword);
        writeToken("openParen", node.openParen);
        writeNode("expr", node.expr);
        writeToken("closeParen", node.closeParen);
        writeNode("item", node.item);
    }
    

    void handle(const RsRuleSyntax& node) {
        writeOptionalNode("randJoin", node.randJoin);
        writeSyntaxList("prods", node.prods);
        writeOptionalNode("weightClause", node.weightClause);
    }
    

    void handle(const RsWeightClauseSyntax& node) {
        writeToken("colonEqual", node.colonEqual);
        writeNode("weight", node.weight);
        writeOptionalNode("codeBlock", node.codeBlock);
    }
    

    void handle(const ScopedNameSyntax& node) {
        writeNode("left", node.left);
        writeToken("separator", node.separator);
        writeNode("right", node.right);
    }
    

    void handle(const SequenceDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeToken("name", node.name);
        writeOptionalNode("portList", node.portList);
        writeToken("semi", node.semi);
        writeSyntaxList("variables", node.variables);
        writeNode("seqExpr", node.seqExpr);
        writeToken("optionalSemi", node.optionalSemi);
        writeToken("end", node.end);
        writeOptionalNode("endBlockName", node.endBlockName);
    }
    

    void handle(const SequenceMatchListSyntax& node) {
        writeToken("comma", node.comma);
        writeSeparatedSyntaxList("items", node.items);
    }
    

    void handle(const SequenceRepetitionSyntax& node) {
        writeToken("openBracket", node.openBracket);
        writeToken("op", node.op);
        writeOptionalNode("selector", node.selector);
        writeToken("closeBracket", node.closeBracket);
    }
    

    void handle(const SignalEventExpressionSyntax& node) {
        writeToken("edge", node.edge);
        writeNode("expr", node.expr);
        writeOptionalNode("iffClause", node.iffClause);
    }
    

    void handle(const SignedCastExpressionSyntax& node) {
        writeToken("signing", node.signing);
        writeToken("apostrophe", node.apostrophe);
        writeNode("inner", node.inner);
    }
    

    void handle(const SimpleAssignmentPatternSyntax& node) {
        writeToken("openBrace", node.openBrace);
        writeSeparatedSyntaxList("items", node.items);
        writeToken("closeBrace", node.closeBrace);
    }
    

    void handle(const SimpleBinsSelectExprSyntax& node) {
        writeNode("expr", node.expr);
        writeOptionalNode("matchesClause", node.matchesClause);
    }
    

    void handle(const SimpleDirectiveSyntax& node) {
        writeToken("directive", node.directive);
    }
    

    void handle(const SimplePathSuffixSyntax& node) {
        writeSeparatedSyntaxList("outputs", node.outputs);
    }
    

    void handle(const SimplePragmaExpressionSyntax& node) {
        writeToken("value", node.value);
    }
    

    void handle(const SimplePropertyExprSyntax& node) {
        writeNode("expr", node.expr);
    }
    

    void handle(const SimpleSequenceExprSyntax& node) {
        writeNode("expr", node.expr);
        writeOptionalNode("repetition", node.repetition);
    }
    

    void handle(const SolveBeforeConstraintSyntax& node) {
        writeToken("solve", node.solve);
        writeSeparatedSyntaxList("beforeExpr", node.beforeExpr);
        writeToken("before", node.before);
        writeSeparatedSyntaxList("afterExpr", node.afterExpr);
        writeToken("semi", node.semi);
    }
    

    void handle(const SpecifyBlockSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("specify", node.specify);
        writeSyntaxList("items", node.items);
        writeToken("endspecify", node.endspecify);
    }
    

    void handle(const SpecparamDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeNode("type", node.type);
        writeSeparatedSyntaxList("declarators", node.declarators);
        writeToken("semi", node.semi);
    }
    

    void handle(const SpecparamDeclaratorSyntax& node) {
        writeToken("name", node.name);
        writeToken("equals", node.equals);
        writeToken("openParen", node.openParen);
        writeNode("value1", node.value1);
        writeToken("comma", node.comma);
        writeOptionalNode("value2", node.value2);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const StandardCaseItemSyntax& node) {
        writeSeparatedSyntaxList("expressions", node.expressions);
        writeToken("colon", node.colon);
        writeNode("clause", node.clause);
    }
    

    void handle(const StandardPropertyCaseItemSyntax& node) {
        writeSeparatedSyntaxList("expressions", node.expressions);
        writeToken("colon", node.colon);
        writeNode("expr", node.expr);
        writeToken("semi", node.semi);
    }
    

    void handle(const StandardRsCaseItemSyntax& node) {
        writeSeparatedSyntaxList("expressions", node.expressions);
        writeToken("colon", node.colon);
        writeNode("item", node.item);
        writeToken("semi", node.semi);
    }
    

    void handle(const StreamExpressionSyntax& node) {
        writeNode("expression", node.expression);
        writeOptionalNode("withRange", node.withRange);
    }
    

    void handle(const StreamExpressionWithRangeSyntax& node) {
        writeToken("withKeyword", node.withKeyword);
        writeNode("range", node.range);
    }
    

    void handle(const StreamingConcatenationExpressionSyntax& node) {
        writeToken("openBrace", node.openBrace);
        writeToken("operatorToken", node.operatorToken);
        writeOptionalNode("sliceSize", node.sliceSize);
        writeToken("innerOpenBrace", node.innerOpenBrace);
        writeSeparatedSyntaxList("expressions", node.expressions);
        writeToken("innerCloseBrace", node.innerCloseBrace);
        writeToken("closeBrace", node.closeBrace);
    }
    

    void handle(const StrongWeakPropertyExprSyntax& node) {
        writeToken("keyword", node.keyword);
        writeToken("openParen", node.openParen);
        writeNode("expr", node.expr);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const StructUnionMemberSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("randomQualifier", node.randomQualifier);
        writeNode("type", node.type);
        writeSeparatedSyntaxList("declarators", node.declarators);
        writeToken("semi", node.semi);
    }
    

    void handle(const StructUnionTypeSyntax& node) {
        writeToken("keyword", node.keyword);
        writeToken("taggedOrSoft", node.taggedOrSoft);
        writeToken("packed", node.packed);
        writeToken("signing", node.signing);
        writeToken("openBrace", node.openBrace);
        writeSyntaxList("members", node.members);
        writeToken("closeBrace", node.closeBrace);
        writeSyntaxList("dimensions", node.dimensions);
    }
    

    void handle(const StructurePatternSyntax& node) {
        writeToken("openBrace", node.openBrace);
        writeSeparatedSyntaxList("members", node.members);
        writeToken("closeBrace", node.closeBrace);
    }
    

    void handle(const StructuredAssignmentPatternSyntax& node) {
        writeToken("openBrace", node.openBrace);
        writeSeparatedSyntaxList("items", node.items);
        writeToken("closeBrace", node.closeBrace);
    }
    

    void handle(const SuperNewDefaultedArgsExpressionSyntax& node) {
        writeNode("scopedNew", node.scopedNew);
        writeToken("openParen", node.openParen);
        writeToken("defaultKeyword", node.defaultKeyword);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const SystemNameSyntax& node) {
        writeToken("systemIdentifier", node.systemIdentifier);
    }
    

    void handle(const SystemTimingCheckSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("name", node.name);
        writeToken("openParen", node.openParen);
        writeSeparatedSyntaxList("args", node.args);
        writeToken("closeParen", node.closeParen);
        writeToken("semi", node.semi);
    }
    

    void handle(const TaggedPatternSyntax& node) {
        writeToken("tagged", node.tagged);
        writeToken("memberName", node.memberName);
        writeOptionalNode("pattern", node.pattern);
    }
    

    void handle(const TaggedUnionExpressionSyntax& node) {
        writeToken("tagged", node.tagged);
        writeToken("member", node.member);
        writeOptionalNode("expr", node.expr);
    }
    

    void handle(const TimeScaleDirectiveSyntax& node) {
        writeToken("directive", node.directive);
        writeToken("timeUnit", node.timeUnit);
        writeToken("slash", node.slash);
        writeToken("timePrecision", node.timePrecision);
    }
    

    void handle(const TimeUnitsDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeToken("time", node.time);
        writeOptionalNode("divider", node.divider);
        writeToken("semi", node.semi);
    }
    

    void handle(const TimingCheckEventArgSyntax& node) {
        writeToken("edge", node.edge);
        writeOptionalNode("controlSpecifier", node.controlSpecifier);
        writeNode("terminal", node.terminal);
        writeOptionalNode("condition", node.condition);
    }
    

    void handle(const TimingCheckEventConditionSyntax& node) {
        writeToken("tripleAnd", node.tripleAnd);
        writeNode("expr", node.expr);
    }
    

    void handle(const TimingControlExpressionSyntax& node) {
        writeNode("timing", node.timing);
        writeNode("expr", node.expr);
    }
    

    void handle(const TimingControlStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeNode("timingControl", node.timingControl);
        writeNode("statement", node.statement);
    }
    

    void handle(const TransListCoverageBinInitializerSyntax& node) {
        writeSeparatedSyntaxList("sets", node.sets);
    }
    

    void handle(const TransRangeSyntax& node) {
        writeSeparatedSyntaxList("items", node.items);
        writeOptionalNode("repeat", node.repeat);
    }
    

    void handle(const TransRepeatRangeSyntax& node) {
        writeToken("openBracket", node.openBracket);
        writeToken("specifier", node.specifier);
        writeOptionalNode("selector", node.selector);
        writeToken("closeBracket", node.closeBracket);
    }
    

    void handle(const TransSetSyntax& node) {
        writeToken("openParen", node.openParen);
        writeSeparatedSyntaxList("ranges", node.ranges);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const TypeAssignmentSyntax& node) {
        writeToken("name", node.name);
        writeOptionalNode("assignment", node.assignment);
    }
    

    void handle(const TypeParameterDeclarationSyntax& node) {
        writeToken("keyword", node.keyword);
        writeToken("typeKeyword", node.typeKeyword);
        writeOptionalNode("typeRestriction", node.typeRestriction);
        writeSeparatedSyntaxList("declarators", node.declarators);
    }
    

    void handle(const TypeReferenceSyntax& node) {
        writeToken("typeKeyword", node.typeKeyword);
        writeToken("openParen", node.openParen);
        writeNode("expr", node.expr);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const TypedefDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("typedefKeyword", node.typedefKeyword);
        writeNode("type", node.type);
        writeToken("name", node.name);
        writeSyntaxList("dimensions", node.dimensions);
        writeToken("semi", node.semi);
    }
    

    void handle(const UdpBodySyntax& node) {
        writeSeparatedSyntaxList("portDecls", node.portDecls);
        writeOptionalNode("initialStmt", node.initialStmt);
        writeToken("table", node.table);
        writeSyntaxList("entries", node.entries);
        writeToken("endtable", node.endtable);
    }
    

    void handle(const UdpDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("primitive", node.primitive);
        writeToken("name", node.name);
        writeNode("portList", node.portList);
        writeNode("body", node.body);
        writeToken("endprimitive", node.endprimitive);
        writeOptionalNode("endBlockName", node.endBlockName);
    }
    

    void handle(const UdpEdgeFieldSyntax& node) {
        writeToken("openParen", node.openParen);
        writeToken("first", node.first);
        writeToken("second", node.second);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const UdpEntrySyntax& node) {
        writeSyntaxList("inputs", node.inputs);
        writeToken("colon1", node.colon1);
        writeOptionalNode("current", node.current);
        writeToken("colon2", node.colon2);
        writeOptionalNode("next", node.next);
        writeToken("semi", node.semi);
    }
    

    void handle(const UdpInitialStmtSyntax& node) {
        writeToken("initial", node.initial);
        writeToken("name", node.name);
        writeToken("equals", node.equals);
        writeNode("value", node.value);
        writeToken("semi", node.semi);
    }
    

    void handle(const UdpInputPortDeclSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeSeparatedSyntaxList("names", node.names);
    }
    

    void handle(const UdpOutputPortDeclSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("keyword", node.keyword);
        writeToken("reg", node.reg);
        writeToken("name", node.name);
        writeOptionalNode("initializer", node.initializer);
    }
    

    void handle(const UdpSimpleFieldSyntax& node) {
        writeToken("field", node.field);
    }
    

    void handle(const UnaryBinsSelectExprSyntax& node) {
        writeToken("op", node.op);
        writeNode("expr", node.expr);
    }
    

    void handle(const UnaryConditionalDirectiveExpressionSyntax& node) {
        writeToken("op", node.op);
        writeNode("operand", node.operand);
    }
    

    void handle(const UnaryPropertyExprSyntax& node) {
        writeToken("op", node.op);
        writeNode("expr", node.expr);
    }
    

    void handle(const UnarySelectPropertyExprSyntax& node) {
        writeToken("op", node.op);
        writeToken("openBracket", node.openBracket);
        writeOptionalNode("selector", node.selector);
        writeToken("closeBracket", node.closeBracket);
        writeNode("expr", node.expr);
    }
    

    void handle(const UnconditionalBranchDirectiveSyntax& node) {
        writeToken("directive", node.directive);
        writeTokenList("disabledTokens", node.disabledTokens);
    }
    

    void handle(const UnconnectedDriveDirectiveSyntax& node) {
        writeToken("directive", node.directive);
        writeToken("strength", node.strength);
    }
    

    void handle(const UndefDirectiveSyntax& node) {
        writeToken("directive", node.directive);
        writeToken("name", node.name);
    }
    

    void handle(const UniquenessConstraintSyntax& node) {
        writeToken("unique", node.unique);
        writeNode("ranges", node.ranges);
        writeToken("semi", node.semi);
    }
    

    void handle(const UserDefinedNetDeclarationSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("netType", node.netType);
        writeNode("delay", node.delay);
        writeSeparatedSyntaxList("declarators", node.declarators);
        writeToken("semi", node.semi);
    }
    

    void handle(const ValueRangeExpressionSyntax& node) {
        writeToken("openBracket", node.openBracket);
        writeNode("left", node.left);
        writeToken("op", node.op);
        writeNode("right", node.right);
        writeToken("closeBracket", node.closeBracket);
    }
    

    void handle(const VariableDimensionSyntax& node) {
        writeToken("openBracket", node.openBracket);
        writeOptionalNode("specifier", node.specifier);
        writeToken("closeBracket", node.closeBracket);
    }
    

    void handle(const VariablePatternSyntax& node) {
        writeToken("dot", node.dot);
        writeToken("variableName", node.variableName);
    }
    

    void handle(const VariablePortHeaderSyntax& node) {
        writeToken("constKeyword", node.constKeyword);
        writeToken("direction", node.direction);
        writeToken("varKeyword", node.varKeyword);
        writeNode("dataType", node.dataType);
    }
    

    void handle(const VirtualInterfaceTypeSyntax& node) {
        writeToken("virtualKeyword", node.virtualKeyword);
        writeToken("interfaceKeyword", node.interfaceKeyword);
        writeToken("name", node.name);
        writeOptionalNode("parameters", node.parameters);
        writeOptionalNode("modport", node.modport);
    }
    

    void handle(const VoidCastedCallStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeToken("voidKeyword", node.voidKeyword);
        writeToken("apostrophe", node.apostrophe);
        writeToken("openParen", node.openParen);
        writeNode("expr", node.expr);
        writeToken("closeParen", node.closeParen);
        writeToken("semi", node.semi);
    }
    

    void handle(const WaitForkStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeToken("wait", node.wait);
        writeToken("fork", node.fork);
        writeToken("semi", node.semi);
    }
    

    void handle(const WaitOrderStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeToken("wait_order", node.wait_order);
        writeToken("openParen", node.openParen);
        writeSeparatedSyntaxList("names", node.names);
        writeToken("closeParen", node.closeParen);
        writeNode("action", node.action);
    }
    

    void handle(const WaitStatementSyntax& node) {
        writeOptionalNode("label", node.label);
        writeSyntaxList("attributes", node.attributes);
        writeToken("wait", node.wait);
        writeToken("openParen", node.openParen);
        writeNode("expr", node.expr);
        writeToken("closeParen", node.closeParen);
        writeNode("statement", node.statement);
    }
    

    void handle(const WildcardDimensionSpecifierSyntax& node) {
        writeToken("star", node.star);
    }
    

    void handle(const WildcardPatternSyntax& node) {
        writeToken("dot", node.dot);
        writeToken("star", node.star);
    }
    

    void handle(const WildcardPortConnectionSyntax& node) {
        writeSyntaxList("attributes", node.attributes);
        writeToken("dot", node.dot);
        writeToken("star", node.star);
    }
    

    void handle(const WildcardPortListSyntax& node) {
        writeToken("openParen", node.openParen);
        writeToken("dot", node.dot);
        writeToken("star", node.star);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const WildcardUdpPortListSyntax& node) {
        writeToken("openParen", node.openParen);
        writeToken("dot", node.dot);
        writeToken("star", node.star);
        writeToken("closeParen", node.closeParen);
        writeToken("semi", node.semi);
    }
    

    void handle(const WithClauseSyntax& node) {
        writeToken("with", node.with);
        writeToken("openParen", node.openParen);
        writeNode("expr", node.expr);
        writeToken("closeParen", node.closeParen);
    }
    

    void handle(const WithFunctionClauseSyntax& node) {
        writeToken("with", node.with);
        writeNode("name", node.name);
    }
    

    void handle(const WithFunctionSampleSyntax& node) {
        writeToken("with", node.with);
        writeToken("function", node.function);
        writeToken("sample", node.sample);
        writeOptionalNode("portList", node.portList);
    }
    
