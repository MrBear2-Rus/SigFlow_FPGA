//------------------------------------------------------------------------------
//! @file TypesDiags.h
//! @brief Generated diagnostic enums for the Types subsystem
//
// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include "slang/diagnostics/Diagnostics.h"

namespace slang::diag {

inline constexpr DiagCode ArrayDimTooLarge(DiagSubsystem::Types, 0);
inline constexpr DiagCode CannotDeclareType(DiagSubsystem::Types, 1);
inline constexpr DiagCode ClassInheritanceCycle(DiagSubsystem::Types, 2);
inline constexpr DiagCode EnumCircularBaseType(DiagSubsystem::Types, 3);
inline constexpr DiagCode EnumIncrementUnknown(DiagSubsystem::Types, 4);
inline constexpr DiagCode EnumRangeMultiDimensional(DiagSubsystem::Types, 5);
inline constexpr DiagCode EnumValueDuplicate(DiagSubsystem::Types, 6);
inline constexpr DiagCode EnumValueOutOfRange(DiagSubsystem::Types, 7);
inline constexpr DiagCode EnumValueOverflow(DiagSubsystem::Types, 8);
inline constexpr DiagCode EnumValueUnknownBits(DiagSubsystem::Types, 9);
inline constexpr DiagCode ForwardTypedefDoesNotMatch(DiagSubsystem::Types, 10);
inline constexpr DiagCode ForwardTypedefVisibility(DiagSubsystem::Types, 11);
inline constexpr DiagCode InvalidArrayElemType(DiagSubsystem::Types, 12);
inline constexpr DiagCode InvalidArraySize(DiagSubsystem::Types, 13);
inline constexpr DiagCode InvalidAssociativeIndexType(DiagSubsystem::Types, 14);
inline constexpr DiagCode InvalidCoverageExpr(DiagSubsystem::Types, 15);
inline constexpr DiagCode InvalidEnumBase(DiagSubsystem::Types, 16);
inline constexpr DiagCode InvalidUnionMember(DiagSubsystem::Types, 17);
inline constexpr DiagCode MultiplePackedOpenArrays(DiagSubsystem::Types, 18);
inline constexpr DiagCode NTResolveClass(DiagSubsystem::Types, 19);
inline constexpr DiagCode NTResolveReturn(DiagSubsystem::Types, 20);
inline constexpr DiagCode NTResolveSingleArg(DiagSubsystem::Types, 21);
inline constexpr DiagCode NTResolveTask(DiagSubsystem::Types, 22);
inline constexpr DiagCode NTResolveUserDef(DiagSubsystem::Types, 23);
inline constexpr DiagCode ObjectTooLarge(DiagSubsystem::Types, 24);
inline constexpr DiagCode PackedArrayNotIntegral(DiagSubsystem::Types, 25);
inline constexpr DiagCode PackedDimsOnPredefinedType(DiagSubsystem::Types, 26);
inline constexpr DiagCode PackedMemberHasInitializer(DiagSubsystem::Types, 27);
inline constexpr DiagCode PackedMemberNotIntegral(DiagSubsystem::Types, 28);
inline constexpr DiagCode PackedTypeTooLarge(DiagSubsystem::Types, 29);
inline constexpr DiagCode PackedUnionWidthMismatch(DiagSubsystem::Types, 30);
inline constexpr DiagCode RecursiveClassSpecialization(DiagSubsystem::Types, 31);
inline constexpr DiagCode TypeRefHierarchical(DiagSubsystem::Types, 32);
inline constexpr DiagCode TypeRefVoid(DiagSubsystem::Types, 33);
inline constexpr DiagCode VirtualInterfaceUnionMember(DiagSubsystem::Types, 34);
inline constexpr DiagCode EnumRangeLiteral(DiagSubsystem::Types, 35);

}
