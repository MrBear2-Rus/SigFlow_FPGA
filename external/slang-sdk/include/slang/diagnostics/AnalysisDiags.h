//------------------------------------------------------------------------------
//! @file AnalysisDiags.h
//! @brief Generated diagnostic enums for the Analysis subsystem
//
// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include "slang/diagnostics/Diagnostics.h"

namespace slang::diag {

inline constexpr DiagCode AlwaysWithoutTimingControl(DiagSubsystem::Analysis, 0);
inline constexpr DiagCode AssertionFormalMultiAssign(DiagSubsystem::Analysis, 1);
inline constexpr DiagCode AssertionFormalUnassigned(DiagSubsystem::Analysis, 2);
inline constexpr DiagCode AssertionLocalUnassigned(DiagSubsystem::Analysis, 3);
inline constexpr DiagCode AssertionNoClock(DiagSubsystem::Analysis, 4);
inline constexpr DiagCode BlockingDelayInTask(DiagSubsystem::Analysis, 5);
inline constexpr DiagCode ClockVarTargetAssign(DiagSubsystem::Analysis, 6);
inline constexpr DiagCode DifferentClockInClockingBlock(DiagSubsystem::Analysis, 7);
inline constexpr DiagCode GFSVMatchItems(DiagSubsystem::Analysis, 8);
inline constexpr DiagCode ImplicitConnNetInconsistent(DiagSubsystem::Analysis, 9);
inline constexpr DiagCode InputPortAssign(DiagSubsystem::Analysis, 10);
inline constexpr DiagCode InterconnectPortVar(DiagSubsystem::Analysis, 11);
inline constexpr DiagCode InvalidMulticlockedSeqOp(DiagSubsystem::Analysis, 12);
inline constexpr DiagCode MismatchedUserDefPortConn(DiagSubsystem::Analysis, 13);
inline constexpr DiagCode MismatchedUserDefPortDir(DiagSubsystem::Analysis, 14);
inline constexpr DiagCode MixedVarAssigns(DiagSubsystem::Analysis, 15);
inline constexpr DiagCode MulticlockedInClockingBlock(DiagSubsystem::Analysis, 16);
inline constexpr DiagCode MulticlockedSeqEmptyMatch(DiagSubsystem::Analysis, 17);
inline constexpr DiagCode MultipleAlwaysAssigns(DiagSubsystem::Analysis, 18);
inline constexpr DiagCode MultipleContAssigns(DiagSubsystem::Analysis, 19);
inline constexpr DiagCode MultipleUDNTDrivers(DiagSubsystem::Analysis, 20);
inline constexpr DiagCode MultipleUWireDrivers(DiagSubsystem::Analysis, 21);
inline constexpr DiagCode NTResolveArgModify(DiagSubsystem::Analysis, 22);
inline constexpr DiagCode NoInferredClock(DiagSubsystem::Analysis, 23);
inline constexpr DiagCode NoUniqueClock(DiagSubsystem::Analysis, 24);
inline constexpr DiagCode SampledValueFuncClock(DiagSubsystem::Analysis, 25);
inline constexpr DiagCode SeqMethodEndClock(DiagSubsystem::Analysis, 26);
inline constexpr DiagCode UserDefPortMixedConcat(DiagSubsystem::Analysis, 27);
inline constexpr DiagCode UserDefPortTwoSided(DiagSubsystem::Analysis, 28);
inline constexpr DiagCode NoteCalledHere(DiagSubsystem::Analysis, 29);
inline constexpr DiagCode NoteClockHere(DiagSubsystem::Analysis, 30);
inline constexpr DiagCode NotePortConnHere(DiagSubsystem::Analysis, 31);
inline constexpr DiagCode NoteRequiredHere(DiagSubsystem::Analysis, 32);
inline constexpr DiagCode CaseComplex(DiagSubsystem::Analysis, 33);
inline constexpr DiagCode CaseDup(DiagSubsystem::Analysis, 34);
inline constexpr DiagCode CaseEnum(DiagSubsystem::Analysis, 35);
inline constexpr DiagCode CaseEnumExplicit(DiagSubsystem::Analysis, 36);
inline constexpr DiagCode CaseIncomplete(DiagSubsystem::Analysis, 37);
inline constexpr DiagCode CaseNone(DiagSubsystem::Analysis, 38);
inline constexpr DiagCode CaseNotWildcard(DiagSubsystem::Analysis, 39);
inline constexpr DiagCode CaseOverlap(DiagSubsystem::Analysis, 40);
inline constexpr DiagCode CaseUnreachable(DiagSubsystem::Analysis, 41);
inline constexpr DiagCode CaseZWithX(DiagSubsystem::Analysis, 42);
inline constexpr DiagCode IncompleteReturn(DiagSubsystem::Analysis, 43);
inline constexpr DiagCode InferredComb(DiagSubsystem::Analysis, 44);
inline constexpr DiagCode InferredLatch(DiagSubsystem::Analysis, 45);
inline constexpr DiagCode InputPortCoercion(DiagSubsystem::Analysis, 46);
inline constexpr DiagCode MissingReturn(DiagSubsystem::Analysis, 47);
inline constexpr DiagCode NetInconsistent(DiagSubsystem::Analysis, 48);
inline constexpr DiagCode NetRangeInconsistent(DiagSubsystem::Analysis, 49);
inline constexpr DiagCode OutputPortCoercion(DiagSubsystem::Analysis, 50);
inline constexpr DiagCode UnassignedVariable(DiagSubsystem::Analysis, 51);
inline constexpr DiagCode UndrivenNet(DiagSubsystem::Analysis, 52);
inline constexpr DiagCode UndrivenPort(DiagSubsystem::Analysis, 53);
inline constexpr DiagCode UnusedArgument(DiagSubsystem::Analysis, 54);
inline constexpr DiagCode UnusedAssertionDecl(DiagSubsystem::Analysis, 55);
inline constexpr DiagCode UnusedButSetNet(DiagSubsystem::Analysis, 56);
inline constexpr DiagCode UnusedButSetPort(DiagSubsystem::Analysis, 57);
inline constexpr DiagCode UnusedButSetVariable(DiagSubsystem::Analysis, 58);
inline constexpr DiagCode UnusedDefinition(DiagSubsystem::Analysis, 59);
inline constexpr DiagCode UnusedGenvar(DiagSubsystem::Analysis, 60);
inline constexpr DiagCode UnusedImplicitNet(DiagSubsystem::Analysis, 61);
inline constexpr DiagCode UnusedImport(DiagSubsystem::Analysis, 62);
inline constexpr DiagCode UnusedNet(DiagSubsystem::Analysis, 63);
inline constexpr DiagCode UnusedParameter(DiagSubsystem::Analysis, 64);
inline constexpr DiagCode UnusedPort(DiagSubsystem::Analysis, 65);
inline constexpr DiagCode UnusedTypeParameter(DiagSubsystem::Analysis, 66);
inline constexpr DiagCode UnusedTypedef(DiagSubsystem::Analysis, 67);
inline constexpr DiagCode UnusedVariable(DiagSubsystem::Analysis, 68);
inline constexpr DiagCode UnusedWildcardImport(DiagSubsystem::Analysis, 69);

}
