// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "EEMemoryTiming.h"

#include <algorithm>

namespace EEMemoryTiming
{
	static constexpr u8 UNMAPPED = 0xff;
	static constexpr u8 UNCACHED_RAM_READ_WAIT = 28;
	static constexpr u32 INSTRUCTION_CACHE_MISS_WAIT = 52;
	static constexpr u32 DATA_CACHE_ENABLE = 1u << 16;
	static constexpr u32 INSTRUCTION_CACHE_ENABLE = 1u << 17;

	struct InstructionCacheTag
	{
		u32 paddr = 0;
		bool valid = false;
		bool lrf = false;
	};

	using InstructionCacheSet = std::array<InstructionCacheTag, 2>;

	std::array<u8, PAGE_COUNT> ReadWaitCycles;
	static std::array<u8, PAGE_COUNT> s_cache_modes;
	static std::array<u32, PAGE_COUNT> s_physical_pages;

	static std::array<InstructionCacheSet, 128> s_instruction_cache;

	static u32 s_config;
	static u32 s_ram_size;

	static u8 ResolveCacheMode(u8 cache_mode)
	{
		return cache_mode == KSEG0_CACHE_MODE ? s_config & 7 : cache_mode;
	}

	static bool IsCacheable(u8 cache_mode)
	{
		return cache_mode == 0 || cache_mode == 3;
	}

	static u8 GetRAMReadWait(u8 cache_mode)
	{
		cache_mode = ResolveCacheMode(cache_mode);

		// C=7 is uncached accelerated: repeated reads can use the read buffer.
		if (cache_mode == UNMAPPED || cache_mode == 7 ||
			(IsCacheable(cache_mode) && (s_config & DATA_CACHE_ENABLE)))
			return 0;

		return UNCACHED_RAM_READ_WAIT;
	}

	void Reset(u32 config, u32 ram_size)
	{
		s_config = config;
		s_ram_size = ram_size;

		s_cache_modes.fill(UNMAPPED);
		s_physical_pages.fill(0);
		ReadWaitCycles.fill(0);

		ResetInstructionCache();
	}

	void SetPage(u32 vaddr, u32 paddr, u8 cache_mode)
	{
		const u32 page = vaddr >> PAGE_BITS;

		s_cache_modes[page] = paddr < s_ram_size ? cache_mode : UNMAPPED;
		s_physical_pages[page] = paddr & ~(PAGE_SIZE - 1);
		ReadWaitCycles[page] = GetRAMReadWait(s_cache_modes[page]);
	}

	void ClearPage(u32 vaddr)
	{
		SetPage(vaddr, 0, UNMAPPED);
	}

	void UpdateConfig(u32 config)
	{
		const u32 changed = (s_config ^ config) & (DATA_CACHE_ENABLE | 7);
		s_config = config;
		if (!changed)
			return;

		for (u32 page = 0; page < PAGE_COUNT; page++)
			ReadWaitCycles[page] = GetRAMReadWait(s_cache_modes[page]);
	}

	void ResetInstructionCache()
	{
		s_instruction_cache = {};
	}

	static bool IsInstructionCacheable(u32 vaddr)
	{
		return IsCacheable(ResolveCacheMode(s_cache_modes[vaddr >> PAGE_BITS]));
	}

	static InstructionCacheSet& GetInstructionCacheSet(u32 vaddr)
	{
		return s_instruction_cache[(vaddr / INSTRUCTION_CACHE_LINE_SIZE) % s_instruction_cache.size()];
	}

	static u32 FillInstructionCache(InstructionCacheSet& ways, u32 way, u32 paddr)
	{
		// Point replacement at the other way, including after an invalid-way refill.
		ways[way] = {paddr, true, way == 0 ? !ways[1].lrf : ways[0].lrf};
		return INSTRUCTION_CACHE_MISS_WAIT;
	}

	u32 FetchInstruction(u32 vaddr)
	{
		if (!(s_config & INSTRUCTION_CACHE_ENABLE) || !IsInstructionCacheable(vaddr))
			return 0;

		auto& ways = GetInstructionCacheSet(vaddr);
		const u32 paddr = s_physical_pages[vaddr >> PAGE_BITS];
		if (std::any_of(ways.begin(), ways.end(), [paddr](const InstructionCacheTag& tag) { return tag.valid && tag.paddr == paddr; }))
			return 0;

		// Misses prefer invalid ways; explicit IFL uses LRF even for invalid ways.
		u32 way = ways[0].lrf ^ ways[1].lrf;
		if (!ways[0].valid)
			way = 0;
		else if (!ways[1].valid)
			way = 1;

		return FillInstructionCache(ways, way, paddr);
	}

	u32 ExecuteInstructionCacheOperation(u8 op, u32 vaddr, u32& tag_lo)
	{
		auto& ways = GetInstructionCacheSet(vaddr);
		auto& tag = ways[vaddr & 1];

		switch (op)
		{
			case 0x00: // IXLTG
				tag_lo = tag.paddr | (static_cast<u32>(tag.valid) << 5) | (static_cast<u32>(tag.lrf) << 4);
				break;

			case 0x04: // IXSTG
				tag = {tag_lo & ~(PAGE_SIZE - 1), (tag_lo & 0x20) != 0, (tag_lo & 0x10) != 0};
				break;

			case 0x07: // IXIN
				tag.valid = false;
				break;

			case 0x0b: // IHIN
				if (!IsInstructionCacheable(vaddr))
					return 0;

				for (auto& entry : ways)
				{
					if (entry.paddr == s_physical_pages[vaddr >> PAGE_BITS])
						entry.valid = false;
				}
				break;

			case 0x0e: // IFL operates even with ICE disabled.
				if (!IsInstructionCacheable(vaddr))
					return 0;

				return FillInstructionCache(ways, ways[0].lrf ^ ways[1].lrf, s_physical_pages[vaddr >> PAGE_BITS]);
		}

		return 0;
	}
} // namespace EEMemoryTiming
