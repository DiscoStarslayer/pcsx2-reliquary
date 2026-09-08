// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

enum class VuUpperFmacSoftKind : u8
{
	Add,
	Sub,
	Mul,
	Madd,
	Msub,
};

enum class VuUpperFmacSoftOperandSource : u8
{
	Ft,
	I,
	Q,
	X,
	Y,
	Z,
	W,
};

enum class VuUpperFmacSoftDestination : u8
{
	Fd,
	Acc,
};

struct alignas(4) VuUpperFmacSoftDescriptor
{
	VuUpperFmacSoftKind kind;
	VuUpperFmacSoftOperandSource source;
	VuUpperFmacSoftDestination destination;

	constexpr bool operator==(const VuUpperFmacSoftDescriptor&) const = default;

	constexpr bool IsKind(VuUpperFmacSoftKind expected_kind) const
	{
		return kind == expected_kind;
	}

	constexpr bool IsMultiplyAdd() const
	{
		return IsKind(VuUpperFmacSoftKind::Madd) || IsKind(VuUpperFmacSoftKind::Msub);
	}

	constexpr bool IsAddSub() const
	{
		return IsKind(VuUpperFmacSoftKind::Add) || IsKind(VuUpperFmacSoftKind::Sub);
	}

	constexpr bool ReadsQ() const
	{
		return source == VuUpperFmacSoftOperandSource::Q;
	}

	constexpr bool UsesBroadcastOperand() const
	{
		return source >= VuUpperFmacSoftOperandSource::X;
	}

	constexpr bool WritesAcc() const
	{
		return destination == VuUpperFmacSoftDestination::Acc;
	}

	constexpr u8 OperandVariant() const
	{
		return static_cast<u8>(source);
	}

};

static_assert(sizeof(VuUpperFmacSoftDescriptor) == 4);
