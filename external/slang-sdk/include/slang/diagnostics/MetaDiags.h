//------------------------------------------------------------------------------
//! @file MetaDiags.h
//! @brief Generated diagnostic enums for the Meta subsystem
//
// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include "slang/diagnostics/Diagnostics.h"

namespace slang::diag {

inline constexpr DiagCode TooManyErrors(DiagSubsystem::Meta, 0);
inline constexpr DiagCode UnknownWarningOption(DiagSubsystem::Meta, 1);

}
