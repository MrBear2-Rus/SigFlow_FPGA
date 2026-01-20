//------------------------------------------------------------------------------
//! @file LookupDiags.h
//! @brief Generated diagnostic enums for the Lookup subsystem
//
// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include "slang/diagnostics/Diagnostics.h"

namespace slang::diag {

inline constexpr DiagCode AmbiguousWildcardImport(DiagSubsystem::Lookup, 0);
inline constexpr DiagCode AutoVariableHierarchical(DiagSubsystem::Lookup, 1);
inline constexpr DiagCode BadInstanceArrayRange(DiagSubsystem::Lookup, 2);
inline constexpr DiagCode CheckerHierarchical(DiagSubsystem::Lookup, 3);
inline constexpr DiagCode CompilationUnitFromPackage(DiagSubsystem::Lookup, 4);
inline constexpr DiagCode CouldNotResolveHierarchicalPath(DiagSubsystem::Lookup, 5);
inline constexpr DiagCode DefinitionUsedAsType(DiagSubsystem::Lookup, 6);
inline constexpr DiagCode DefinitionUsedAsValue(DiagSubsystem::Lookup, 7);
inline constexpr DiagCode DotIntoInstArray(DiagSubsystem::Lookup, 8);
inline constexpr DiagCode DotOnType(DiagSubsystem::Lookup, 9);
inline constexpr DiagCode GenericClassScopeResolution(DiagSubsystem::Lookup, 10);
inline constexpr DiagCode HierarchicalFromPackage(DiagSubsystem::Lookup, 11);
inline constexpr DiagCode HierarchicalRefUnknownModule(DiagSubsystem::Lookup, 12);
inline constexpr DiagCode IfaceExtendIncomplete(DiagSubsystem::Lookup, 13);
inline constexpr DiagCode IfaceExtendTypeParam(DiagSubsystem::Lookup, 14);
inline constexpr DiagCode IfaceImportExportTarget(DiagSubsystem::Lookup, 15);
inline constexpr DiagCode IllegalReferenceToProgramItem(DiagSubsystem::Lookup, 16);
inline constexpr DiagCode ImportNameCollision(DiagSubsystem::Lookup, 17);
inline constexpr DiagCode InstanceArrayEndianMismatch(DiagSubsystem::Lookup, 18);
inline constexpr DiagCode InvalidConstructorAccess(DiagSubsystem::Lookup, 19);
inline constexpr DiagCode InvalidHierarchicalIfacePortConn(DiagSubsystem::Lookup, 20);
inline constexpr DiagCode InvalidModportAccess(DiagSubsystem::Lookup, 21);
inline constexpr DiagCode InvalidScopeIndexExpression(DiagSubsystem::Lookup, 22);
inline constexpr DiagCode InvalidThisHandle(DiagSubsystem::Lookup, 23);
inline constexpr DiagCode LocalMemberAccess(DiagSubsystem::Lookup, 24);
inline constexpr DiagCode LocalNotAllowed(DiagSubsystem::Lookup, 25);
inline constexpr DiagCode NestedNonStaticClassMethod(DiagSubsystem::Lookup, 26);
inline constexpr DiagCode NestedNonStaticClassProperty(DiagSubsystem::Lookup, 27);
inline constexpr DiagCode NoDefaultSpecialization(DiagSubsystem::Lookup, 28);
inline constexpr DiagCode NonStaticClassMethod(DiagSubsystem::Lookup, 29);
inline constexpr DiagCode NonStaticClassProperty(DiagSubsystem::Lookup, 30);
inline constexpr DiagCode NotAClass(DiagSubsystem::Lookup, 31);
inline constexpr DiagCode NotAGenericClass(DiagSubsystem::Lookup, 32);
inline constexpr DiagCode NotAGenvar(DiagSubsystem::Lookup, 33);
inline constexpr DiagCode NotAHierarchicalScope(DiagSubsystem::Lookup, 34);
inline constexpr DiagCode NotAModport(DiagSubsystem::Lookup, 35);
inline constexpr DiagCode NotASubroutine(DiagSubsystem::Lookup, 36);
inline constexpr DiagCode NotAType(DiagSubsystem::Lookup, 37);
inline constexpr DiagCode NotAValue(DiagSubsystem::Lookup, 38);
inline constexpr DiagCode NotAnInterface(DiagSubsystem::Lookup, 39);
inline constexpr DiagCode NotAnInterfaceOrPort(DiagSubsystem::Lookup, 40);
inline constexpr DiagCode ProtectedMemberAccess(DiagSubsystem::Lookup, 41);
inline constexpr DiagCode RecursiveDefinition(DiagSubsystem::Lookup, 42);
inline constexpr DiagCode Redefinition(DiagSubsystem::Lookup, 43);
inline constexpr DiagCode RedefinitionDifferentType(DiagSubsystem::Lookup, 44);
inline constexpr DiagCode ScopeIncompleteTypedef(DiagSubsystem::Lookup, 45);
inline constexpr DiagCode ScopeIndexOutOfRange(DiagSubsystem::Lookup, 46);
inline constexpr DiagCode ScopeNotIndexable(DiagSubsystem::Lookup, 47);
inline constexpr DiagCode SuperNoBase(DiagSubsystem::Lookup, 48);
inline constexpr DiagCode SuperOutsideClass(DiagSubsystem::Lookup, 49);
inline constexpr DiagCode TypeHierarchical(DiagSubsystem::Lookup, 50);
inline constexpr DiagCode TypoIdentifier(DiagSubsystem::Lookup, 51);
inline constexpr DiagCode UndeclaredButFoundPackage(DiagSubsystem::Lookup, 52);
inline constexpr DiagCode UndeclaredIdentifier(DiagSubsystem::Lookup, 53);
inline constexpr DiagCode UnexpectedNameToken(DiagSubsystem::Lookup, 54);
inline constexpr DiagCode UnexpectedSelection(DiagSubsystem::Lookup, 55);
inline constexpr DiagCode UnknownClassMember(DiagSubsystem::Lookup, 56);
inline constexpr DiagCode UnknownClassOrPackage(DiagSubsystem::Lookup, 57);
inline constexpr DiagCode UnknownCovergroupMember(DiagSubsystem::Lookup, 58);
inline constexpr DiagCode UnknownMember(DiagSubsystem::Lookup, 59);
inline constexpr DiagCode UnknownPackageMember(DiagSubsystem::Lookup, 60);
inline constexpr DiagCode UnknownSystemMethod(DiagSubsystem::Lookup, 61);
inline constexpr DiagCode UnnamedGenerateReference(DiagSubsystem::Lookup, 62);
inline constexpr DiagCode UnresolvedForwardTypedef(DiagSubsystem::Lookup, 63);
inline constexpr DiagCode UsedBeforeDeclared(DiagSubsystem::Lookup, 64);
inline constexpr DiagCode NoteImportedFrom(DiagSubsystem::Lookup, 65);
inline constexpr DiagCode DuplicateDefinition(DiagSubsystem::Lookup, 66);
inline constexpr DiagCode DuplicateImport(DiagSubsystem::Lookup, 67);
inline constexpr DiagCode UnknownSystemName(DiagSubsystem::Lookup, 68);

}
