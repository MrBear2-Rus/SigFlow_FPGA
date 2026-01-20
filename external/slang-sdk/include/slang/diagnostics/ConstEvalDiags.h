//------------------------------------------------------------------------------
//! @file ConstEvalDiags.h
//! @brief Generated diagnostic enums for the ConstEval subsystem
//
// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include "slang/diagnostics/Diagnostics.h"

namespace slang::diag {

inline constexpr DiagCode ConstEvalAssertionFailed(DiagSubsystem::ConstEval, 0);
inline constexpr DiagCode ConstEvalAssociativeIndexInvalid(DiagSubsystem::ConstEval, 1);
inline constexpr DiagCode ConstEvalBitstreamCastSize(DiagSubsystem::ConstEval, 2);
inline constexpr DiagCode ConstEvalCaseItemsNotUnique(DiagSubsystem::ConstEval, 3);
inline constexpr DiagCode ConstEvalCheckers(DiagSubsystem::ConstEval, 4);
inline constexpr DiagCode ConstEvalClassType(DiagSubsystem::ConstEval, 5);
inline constexpr DiagCode ConstEvalCovergroupType(DiagSubsystem::ConstEval, 6);
inline constexpr DiagCode ConstEvalDPINotConstant(DiagSubsystem::ConstEval, 7);
inline constexpr DiagCode ConstEvalDisableTarget(DiagSubsystem::ConstEval, 8);
inline constexpr DiagCode ConstEvalDynamicToFixedSize(DiagSubsystem::ConstEval, 9);
inline constexpr DiagCode ConstEvalExceededMaxCallDepth(DiagSubsystem::ConstEval, 10);
inline constexpr DiagCode ConstEvalExceededMaxSteps(DiagSubsystem::ConstEval, 11);
inline constexpr DiagCode ConstEvalFunctionArgDirection(DiagSubsystem::ConstEval, 12);
inline constexpr DiagCode ConstEvalFunctionIdentifiersMustBeLocal(DiagSubsystem::ConstEval, 13);
inline constexpr DiagCode ConstEvalFunctionInsideGenerate(DiagSubsystem::ConstEval, 14);
inline constexpr DiagCode ConstEvalHierarchicalName(DiagSubsystem::ConstEval, 15);
inline constexpr DiagCode ConstEvalIdUsedInCEBeforeDecl(DiagSubsystem::ConstEval, 16);
inline constexpr DiagCode ConstEvalIfItemsNotUnique(DiagSubsystem::ConstEval, 17);
inline constexpr DiagCode ConstEvalMethodNotConstant(DiagSubsystem::ConstEval, 18);
inline constexpr DiagCode ConstEvalNoCaseItemsMatched(DiagSubsystem::ConstEval, 19);
inline constexpr DiagCode ConstEvalNoIfItemsMatched(DiagSubsystem::ConstEval, 20);
inline constexpr DiagCode ConstEvalNonConstVariable(DiagSubsystem::ConstEval, 21);
inline constexpr DiagCode ConstEvalParallelBlockNotConst(DiagSubsystem::ConstEval, 22);
inline constexpr DiagCode ConstEvalParamCycle(DiagSubsystem::ConstEval, 23);
inline constexpr DiagCode ConstEvalProceduralAssign(DiagSubsystem::ConstEval, 24);
inline constexpr DiagCode ConstEvalRandValue(DiagSubsystem::ConstEval, 25);
inline constexpr DiagCode ConstEvalReplicationCountInvalid(DiagSubsystem::ConstEval, 26);
inline constexpr DiagCode ConstEvalSubroutineNotConstant(DiagSubsystem::ConstEval, 27);
inline constexpr DiagCode ConstEvalTaggedUnion(DiagSubsystem::ConstEval, 28);
inline constexpr DiagCode ConstEvalTaskNotConstant(DiagSubsystem::ConstEval, 29);
inline constexpr DiagCode ConstEvalTimedStmtNotConst(DiagSubsystem::ConstEval, 30);
inline constexpr DiagCode ConstEvalVoidNotConstant(DiagSubsystem::ConstEval, 31);
inline constexpr DiagCode NoteInCallTo(DiagSubsystem::ConstEval, 32);
inline constexpr DiagCode NoteSkippingFrames(DiagSubsystem::ConstEval, 33);
inline constexpr DiagCode ConstEvalAssociativeElementNotFound(DiagSubsystem::ConstEval, 34);
inline constexpr DiagCode ConstEvalDynamicArrayIndex(DiagSubsystem::ConstEval, 35);
inline constexpr DiagCode ConstEvalDynamicArrayRange(DiagSubsystem::ConstEval, 36);
inline constexpr DiagCode ConstEvalEmptyQueue(DiagSubsystem::ConstEval, 37);
inline constexpr DiagCode ConstEvalQueueRange(DiagSubsystem::ConstEval, 38);
inline constexpr DiagCode ConstEvalStaticSkipped(DiagSubsystem::ConstEval, 39);
inline constexpr DiagCode ConstSysTaskIgnored(DiagSubsystem::ConstEval, 40);
inline constexpr DiagCode ConstantConversion(DiagSubsystem::ConstEval, 41);

}
