//------------------------------------------------------------------------------
//! @file PreprocessorDiags.h
//! @brief Generated diagnostic enums for the Preprocessor subsystem
//
// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include "slang/diagnostics/Diagnostics.h"

namespace slang::diag {

inline constexpr DiagCode CouldNotOpenIncludeFile(DiagSubsystem::Preprocessor, 0);
inline constexpr DiagCode DirectiveInsideDesignElement(DiagSubsystem::Preprocessor, 1);
inline constexpr DiagCode ExceededMaxIncludeDepth(DiagSubsystem::Preprocessor, 2);
inline constexpr DiagCode ExpectedDriveStrength(DiagSubsystem::Preprocessor, 3);
inline constexpr DiagCode ExpectedIncludeFileName(DiagSubsystem::Preprocessor, 4);
inline constexpr DiagCode ExpectedMacroArgs(DiagSubsystem::Preprocessor, 5);
inline constexpr DiagCode ExpectedMacroCommentEnd(DiagSubsystem::Preprocessor, 6);
inline constexpr DiagCode ExpectedMacroStringifyEnd(DiagSubsystem::Preprocessor, 7);
inline constexpr DiagCode ExpectedNetType(DiagSubsystem::Preprocessor, 8);
inline constexpr DiagCode ExpectedPragmaExpression(DiagSubsystem::Preprocessor, 9);
inline constexpr DiagCode ExpectedPragmaName(DiagSubsystem::Preprocessor, 10);
inline constexpr DiagCode ExpectedTimeLiteral(DiagSubsystem::Preprocessor, 11);
inline constexpr DiagCode InvalidInferredTimeScale(DiagSubsystem::Preprocessor, 12);
inline constexpr DiagCode InvalidLineDirectiveLevel(DiagSubsystem::Preprocessor, 13);
inline constexpr DiagCode InvalidMacroName(DiagSubsystem::Preprocessor, 14);
inline constexpr DiagCode InvalidTimeScalePrecision(DiagSubsystem::Preprocessor, 15);
inline constexpr DiagCode InvalidTimeScaleSpecifier(DiagSubsystem::Preprocessor, 16);
inline constexpr DiagCode MacroOpsOutsideDefinition(DiagSubsystem::Preprocessor, 17);
inline constexpr DiagCode MacroTokensAfterPragmaProtect(DiagSubsystem::Preprocessor, 18);
inline constexpr DiagCode MismatchedEndKeywordsDirective(DiagSubsystem::Preprocessor, 19);
inline constexpr DiagCode MissingEndIfDirective(DiagSubsystem::Preprocessor, 20);
inline constexpr DiagCode NotEnoughMacroArgs(DiagSubsystem::Preprocessor, 21);
inline constexpr DiagCode RecursiveMacro(DiagSubsystem::Preprocessor, 22);
inline constexpr DiagCode TooManyActualMacroArgs(DiagSubsystem::Preprocessor, 23);
inline constexpr DiagCode UnbalancedMacroArgDims(DiagSubsystem::Preprocessor, 24);
inline constexpr DiagCode UndefineBuiltinDirective(DiagSubsystem::Preprocessor, 25);
inline constexpr DiagCode UnexpectedConditionalDirective(DiagSubsystem::Preprocessor, 26);
inline constexpr DiagCode UnknownDirective(DiagSubsystem::Preprocessor, 27);
inline constexpr DiagCode UnrecognizedKeywordVersion(DiagSubsystem::Preprocessor, 28);
inline constexpr DiagCode NoteDirectiveHere(DiagSubsystem::Preprocessor, 29);
inline constexpr DiagCode ExpectedDiagPragmaArg(DiagSubsystem::Preprocessor, 30);
inline constexpr DiagCode ExpectedDiagPragmaLevel(DiagSubsystem::Preprocessor, 31);
inline constexpr DiagCode ExpectedProtectArg(DiagSubsystem::Preprocessor, 32);
inline constexpr DiagCode ExpectedProtectKeyword(DiagSubsystem::Preprocessor, 33);
inline constexpr DiagCode ExtraPragmaArgs(DiagSubsystem::Preprocessor, 34);
inline constexpr DiagCode ExtraProtectEnd(DiagSubsystem::Preprocessor, 35);
inline constexpr DiagCode IgnoredMacroPaste(DiagSubsystem::Preprocessor, 36);
inline constexpr DiagCode InvalidPragmaNumber(DiagSubsystem::Preprocessor, 37);
inline constexpr DiagCode InvalidPragmaViewport(DiagSubsystem::Preprocessor, 38);
inline constexpr DiagCode NestedProtectBegin(DiagSubsystem::Preprocessor, 39);
inline constexpr DiagCode ProtectArgList(DiagSubsystem::Preprocessor, 40);
inline constexpr DiagCode ProtectedEnvelope(DiagSubsystem::Preprocessor, 41);
inline constexpr DiagCode RedefiningMacro(DiagSubsystem::Preprocessor, 42);
inline constexpr DiagCode UnknownDiagPragmaArg(DiagSubsystem::Preprocessor, 43);
inline constexpr DiagCode UnknownPragma(DiagSubsystem::Preprocessor, 44);
inline constexpr DiagCode UnknownProtectEncoding(DiagSubsystem::Preprocessor, 45);
inline constexpr DiagCode UnknownProtectKeyword(DiagSubsystem::Preprocessor, 46);
inline constexpr DiagCode UnknownProtectOption(DiagSubsystem::Preprocessor, 47);

}
