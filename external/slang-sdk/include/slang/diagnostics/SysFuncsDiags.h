//------------------------------------------------------------------------------
//! @file SysFuncsDiags.h
//! @brief Generated diagnostic enums for the SysFuncs subsystem
//
// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include "slang/diagnostics/Diagnostics.h"

namespace slang::diag {

inline constexpr DiagCode ArrayLocatorWithClause(DiagSubsystem::SysFuncs, 0);
inline constexpr DiagCode ArrayMethodComparable(DiagSubsystem::SysFuncs, 1);
inline constexpr DiagCode ArrayMethodIntegral(DiagSubsystem::SysFuncs, 2);
inline constexpr DiagCode AssociativeWildcardNotAllowed(DiagSubsystem::SysFuncs, 3);
inline constexpr DiagCode AutoVarTraced(DiagSubsystem::SysFuncs, 4);
inline constexpr DiagCode BadSystemSubroutineArg(DiagSubsystem::SysFuncs, 5);
inline constexpr DiagCode DimensionIndexInvalid(DiagSubsystem::SysFuncs, 6);
inline constexpr DiagCode DynamicDimensionIndex(DiagSubsystem::SysFuncs, 7);
inline constexpr DiagCode ExpectedClassPropertyName(DiagSubsystem::SysFuncs, 8);
inline constexpr DiagCode ExpectedModOrVarName(DiagSubsystem::SysFuncs, 9);
inline constexpr DiagCode ExpectedModuleInstance(DiagSubsystem::SysFuncs, 10);
inline constexpr DiagCode ExpectedModuleName(DiagSubsystem::SysFuncs, 11);
inline constexpr DiagCode ExpectedNetRef(DiagSubsystem::SysFuncs, 12);
inline constexpr DiagCode ExpectedScopeName(DiagSubsystem::SysFuncs, 13);
inline constexpr DiagCode ExpectedScopeOrAssert(DiagSubsystem::SysFuncs, 14);
inline constexpr DiagCode ExpectedVariableName(DiagSubsystem::SysFuncs, 15);
inline constexpr DiagCode FormatEmptyArg(DiagSubsystem::SysFuncs, 16);
inline constexpr DiagCode FormatMismatchedType(DiagSubsystem::SysFuncs, 17);
inline constexpr DiagCode FormatNoArgument(DiagSubsystem::SysFuncs, 18);
inline constexpr DiagCode FormatSpecifierInvalidWidth(DiagSubsystem::SysFuncs, 19);
inline constexpr DiagCode FormatSpecifierNotFloat(DiagSubsystem::SysFuncs, 20);
inline constexpr DiagCode FormatSpecifierWidthNotAllowed(DiagSubsystem::SysFuncs, 21);
inline constexpr DiagCode FormatTooManyArgs(DiagSubsystem::SysFuncs, 22);
inline constexpr DiagCode FormatUnspecifiedType(DiagSubsystem::SysFuncs, 23);
inline constexpr DiagCode GlobalClockEventExpr(DiagSubsystem::SysFuncs, 24);
inline constexpr DiagCode GlobalSampledValueAssertionExpr(DiagSubsystem::SysFuncs, 25);
inline constexpr DiagCode InferredValDefArg(DiagSubsystem::SysFuncs, 26);
inline constexpr DiagCode InvalidStringArg(DiagSubsystem::SysFuncs, 27);
inline constexpr DiagCode NoGlobalClocking(DiagSubsystem::SysFuncs, 28);
inline constexpr DiagCode PastNumTicksInvalid(DiagSubsystem::SysFuncs, 29);
inline constexpr DiagCode PlaRangeInAscendingOrder(DiagSubsystem::SysFuncs, 30);
inline constexpr DiagCode QueryOnAssociativeNonIntegral(DiagSubsystem::SysFuncs, 31);
inline constexpr DiagCode QueryOnAssociativeWildcard(DiagSubsystem::SysFuncs, 32);
inline constexpr DiagCode QueryOnDynamicType(DiagSubsystem::SysFuncs, 33);
inline constexpr DiagCode SampledValueLocalVar(DiagSubsystem::SysFuncs, 34);
inline constexpr DiagCode SampledValueMatched(DiagSubsystem::SysFuncs, 35);
inline constexpr DiagCode SeqMethodInputLocalVar(DiagSubsystem::SysFuncs, 36);
inline constexpr DiagCode SequenceMatchedOutsideAssertion(DiagSubsystem::SysFuncs, 37);
inline constexpr DiagCode SequenceMethodLocalVar(DiagSubsystem::SysFuncs, 38);
inline constexpr DiagCode SysFuncHierarchicalNotAllowed(DiagSubsystem::SysFuncs, 39);
inline constexpr DiagCode SysFuncNotConst(DiagSubsystem::SysFuncs, 40);
inline constexpr DiagCode TypeIsNotAClass(DiagSubsystem::SysFuncs, 41);
inline constexpr DiagCode UnknownFormatSpecifier(DiagSubsystem::SysFuncs, 42);
inline constexpr DiagCode BadFinishNum(DiagSubsystem::SysFuncs, 43);
inline constexpr DiagCode FormatMultibitStrength(DiagSubsystem::SysFuncs, 44);
inline constexpr DiagCode FormatRealInt(DiagSubsystem::SysFuncs, 45);
inline constexpr DiagCode IsUnboundedParamArg(DiagSubsystem::SysFuncs, 46);
inline constexpr DiagCode MissingFormatSpecifier(DiagSubsystem::SysFuncs, 47);
inline constexpr DiagCode NonstandardSysFunc(DiagSubsystem::SysFuncs, 48);

}
