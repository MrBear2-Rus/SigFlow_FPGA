//------------------------------------------------------------------------------
//! @file GeneralDiags.h
//! @brief Generated diagnostic enums for the General subsystem
//
// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include "slang/diagnostics/Diagnostics.h"

namespace slang::diag {

inline constexpr DiagCode AttributesNotAllowed(DiagSubsystem::General, 0);
inline constexpr DiagCode ExpectedExpression(DiagSubsystem::General, 1);
inline constexpr DiagCode WrongLanguageVersion(DiagSubsystem::General, 2);
inline constexpr DiagCode NoteAssignedHere(DiagSubsystem::General, 3);
inline constexpr DiagCode NoteConfigRule(DiagSubsystem::General, 4);
inline constexpr DiagCode NoteDeclarationHere(DiagSubsystem::General, 5);
inline constexpr DiagCode NoteDrivenHere(DiagSubsystem::General, 6);
inline constexpr DiagCode NoteExpandedHere(DiagSubsystem::General, 7);
inline constexpr DiagCode NoteFromHere2(DiagSubsystem::General, 8);
inline constexpr DiagCode NoteOriginalAssign(DiagSubsystem::General, 9);
inline constexpr DiagCode NotePreviousDefinition(DiagSubsystem::General, 10);
inline constexpr DiagCode NotePreviousMatch(DiagSubsystem::General, 11);
inline constexpr DiagCode NotePreviousUsage(DiagSubsystem::General, 12);
inline constexpr DiagCode NoteReferencedHere(DiagSubsystem::General, 13);

}
