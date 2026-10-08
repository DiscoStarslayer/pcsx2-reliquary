// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

namespace VUCommunication
{
	constexpr u8 NormalizeStatusLow(u16 raw)
	{
		return ((raw & 0xf) ? 0x40 : 0) | ((raw & 0xf0) ? 0x80 : 0) |
		       ((raw & 0xf00) ? 1 : 0) | ((raw & 0xf000) ? 2 : 0);
	}

	// At least one unit must be active. An inactive unit cannot delay its partner.
	constexpr u32 SelectUnit(bool active0, bool active1, u64 cycle0, u64 cycle1)
	{
		return active0 && (!active1 || cycle0 <= cycle1) ? 0 : 1;
	}

	// A missing or future version is not an architectural read instance. In that
	// case the caller retains the previously published mature value.
	constexpr int FindMatureFlagSlot(const int* ready_cycles, int cycle)
	{
		int slot = -1;
		int newest = -1;
		for (int i = 0; i < 4; i++)
		{
			if (ready_cycles[i] <= cycle && ready_cycles[i] > newest)
			{
				slot = i;
				newest = ready_cycles[i];
			}
		}
		return slot;
	}
} // namespace VUCommunication
