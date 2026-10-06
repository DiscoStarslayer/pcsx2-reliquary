// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <array>

// CPU-only timing: memory accessors are also used by DMA and the debugger.
namespace EEMemoryTiming
{
	inline constexpr u32 PAGE_BITS = 12;
	inline constexpr u32 PAGE_SIZE = 1u << PAGE_BITS;
	inline constexpr u32 PAGE_COUNT = 1u << (32 - PAGE_BITS);

	inline constexpr u8 KSEG0_CACHE_MODE = 8;
	inline constexpr u32 INSTRUCTION_CACHE_LINE_SIZE = 64;

	extern std::array<u8, PAGE_COUNT> ReadWaitCycles;

	void Reset(u32 config, u32 ram_size);
	void SetPage(u32 vaddr, u32 paddr, u8 cache_mode);
	void ClearPage(u32 vaddr);
	void UpdateConfig(u32 config);

	void ResetInstructionCache();
	u32 FetchInstruction(u32 vaddr);
	u32 ExecuteInstructionCacheOperation(u8 op, u32 vaddr, u32& tag_lo);

	constexpr bool IsInstructionCacheOperation(u8 op)
	{
		return op == 0x00 || op == 0x04 || op == 0x07 || op == 0x0b || op == 0x0e;
	}
} // namespace EEMemoryTiming
