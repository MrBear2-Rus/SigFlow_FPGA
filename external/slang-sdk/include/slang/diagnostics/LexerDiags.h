//------------------------------------------------------------------------------
//! @file LexerDiags.h
//! @brief Generated diagnostic enums for the Lexer subsystem
//
// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include "slang/diagnostics/Diagnostics.h"

namespace slang::diag {

inline constexpr DiagCode BadBinaryDigit(DiagSubsystem::Lexer, 0);
inline constexpr DiagCode BadDecimalDigit(DiagSubsystem::Lexer, 1);
inline constexpr DiagCode BadHexDigit(DiagSubsystem::Lexer, 2);
inline constexpr DiagCode BadOctalDigit(DiagSubsystem::Lexer, 3);
inline constexpr DiagCode DecimalDigitMultipleUnknown(DiagSubsystem::Lexer, 4);
inline constexpr DiagCode EmbeddedNull(DiagSubsystem::Lexer, 5);
inline constexpr DiagCode EscapedWhitespace(DiagSubsystem::Lexer, 6);
inline constexpr DiagCode ExpectedClosingQuote(DiagSubsystem::Lexer, 7);
inline constexpr DiagCode ExpectedIntegerBaseAfterSigned(DiagSubsystem::Lexer, 8);
inline constexpr DiagCode InvalidHexEscapeCode(DiagSubsystem::Lexer, 9);
inline constexpr DiagCode MisplacedDirectiveChar(DiagSubsystem::Lexer, 10);
inline constexpr DiagCode NestedBlockComment(DiagSubsystem::Lexer, 11);
inline constexpr DiagCode NonPrintableChar(DiagSubsystem::Lexer, 12);
inline constexpr DiagCode OctalEscapeCodeTooBig(DiagSubsystem::Lexer, 13);
inline constexpr DiagCode TooManyLexerErrors(DiagSubsystem::Lexer, 14);
inline constexpr DiagCode UTF8Char(DiagSubsystem::Lexer, 15);
inline constexpr DiagCode UnclosedTranslateOff(DiagSubsystem::Lexer, 16);
inline constexpr DiagCode UnicodeBOM(DiagSubsystem::Lexer, 17);
inline constexpr DiagCode UnterminatedBlockComment(DiagSubsystem::Lexer, 18);
inline constexpr DiagCode InvalidEncodingByte(DiagSubsystem::Lexer, 19);
inline constexpr DiagCode InvalidUTF8Seq(DiagSubsystem::Lexer, 20);
inline constexpr DiagCode NonstandardEscapeCode(DiagSubsystem::Lexer, 21);
inline constexpr DiagCode ProtectEncodingBytes(DiagSubsystem::Lexer, 22);
inline constexpr DiagCode RawProtectEOF(DiagSubsystem::Lexer, 23);
inline constexpr DiagCode UnknownEscapeCode(DiagSubsystem::Lexer, 24);

}
