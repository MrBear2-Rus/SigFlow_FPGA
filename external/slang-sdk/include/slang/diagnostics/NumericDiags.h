//------------------------------------------------------------------------------
//! @file NumericDiags.h
//! @brief Generated diagnostic enums for the Numeric subsystem
//
// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include "slang/diagnostics/Diagnostics.h"

namespace slang::diag {

inline constexpr DiagCode DigitsLeadingUnderscore(DiagSubsystem::Numeric, 0);
inline constexpr DiagCode ExpectedVectorDigits(DiagSubsystem::Numeric, 1);
inline constexpr DiagCode LiteralSizeIsZero(DiagSubsystem::Numeric, 2);
inline constexpr DiagCode LiteralSizeTooLarge(DiagSubsystem::Numeric, 3);
inline constexpr DiagCode MissingExponentDigits(DiagSubsystem::Numeric, 4);
inline constexpr DiagCode MissingFractionalDigits(DiagSubsystem::Numeric, 5);
inline constexpr DiagCode ValueExceedsMaxBitWidth(DiagSubsystem::Numeric, 6);
inline constexpr DiagCode ValueMustBeIntegral(DiagSubsystem::Numeric, 7);
inline constexpr DiagCode ValueMustBePositive(DiagSubsystem::Numeric, 8);
inline constexpr DiagCode ValueMustNotBeUnknown(DiagSubsystem::Numeric, 9);
inline constexpr DiagCode ValueOutOfRange(DiagSubsystem::Numeric, 10);
inline constexpr DiagCode RealLiteralOverflow(DiagSubsystem::Numeric, 11);
inline constexpr DiagCode RealLiteralUnderflow(DiagSubsystem::Numeric, 12);
inline constexpr DiagCode SignedIntegerOverflow(DiagSubsystem::Numeric, 13);
inline constexpr DiagCode VectorLiteralOverflow(DiagSubsystem::Numeric, 14);

}
