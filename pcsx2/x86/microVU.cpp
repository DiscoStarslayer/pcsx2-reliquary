// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "microVU.h"

#include "VUops.h"

#include "common/AlignedMalloc.h"

#include <array>

void* g_mvuPreparedEntry[2] = {};
MvuCommunicationRequest g_mvuCommunicationRequest;
MvuCommunicationHint g_mvuCommunicationHint[2];
void* g_mvuCommunicationBody[2] = {};
void* g_mvuCommunicationHotBody[2] = {};

const u8* mVUstatusTable()
{
	static const std::array<u8, 65536> table = [] {
		std::array<u8, 65536> values{};
		for (u32 raw = 0; raw < values.size(); raw++)
			values[raw] = VUCommunication::NormalizeStatusLow(static_cast<u16>(raw));
		return values;
	}();
	return table.data();
}

static constexpr size_t mVUsoftDivCapTailReserve = 256;

//------------------------------------------------------------------
// Micro VU - Main Functions
//------------------------------------------------------------------

// Only run this once per VU! ;)
void mVUinit(microVU& mVU, uint vuIndex)
{
	std::memset(&mVU.prog, 0, sizeof(mVU.prog));

	mVU.index        =  vuIndex;
	mVU.cop2         =  0;
	mVU.vuMemSize    = (mVU.index ? 0x4000 : 0x1000);
	mVU.microMemSize = (mVU.index ? 0x4000 : 0x1000);
	mVU.progSize     = (mVU.index ? 0x4000 : 0x1000) / 4;
	mVU.progMemMask  =  mVU.progSize-1;
	mVU.cache        = vuIndex ? SysMemory::GetVU1Rec() : SysMemory::GetVU0Rec();
	mVU.prog.x86end  = (vuIndex ? SysMemory::GetVU1RecEnd() : SysMemory::GetVU0RecEnd()) - (mVUcacheSafeZone * _1mb);
	// Keep the cold DIV cap fallback outside normal program allocation.
	mVU.prog.x86end -= mVUsoftDivCapTailReserve;

	mVU.regAlloc.reset(new microRegAlloc(mVU.index));
}

// Resets Rec Data
void mVUreset(microVU& mVU)
{
	g_mvuCommunicationHint[mVU.index] = {};
	g_mvuPreparedEntry[mVU.index] = nullptr;
	const bool use_soft_float = CHECK_VU_SOFT(mVU.index);
	const bool use_soft_madd_packed =
		use_soft_float && g_cpu.vectorISA >= ProcessorFeatures::VectorISA::AVX2;

	if (use_soft_float)
	{
		MicroVUSoftFloatTables::InitializeCorrectionTables();

		if (!mVU.softBoothCache)
			mVU.softBoothCache = std::make_unique<microVUSoftBoothCacheEntry[]>(mVUsoftBoothCacheSize);
		if (!mVU.softSrtReciprocalCache)
			mVU.softSrtReciprocalCache =
				std::make_unique<microVUSoftUnaryCacheEntry[]>(mVUsoftLowerCacheSize);
		if (!mVU.softDivCache)
			mVU.softDivCache = std::make_unique<microVUSoftLowerCacheEntry[]>(mVUsoftLowerCacheSize);
		if (!mVU.softSqrtCache)
			mVU.softSqrtCache = std::make_unique<microVUSoftUnaryCacheEntry[]>(mVUsoftLowerCacheSize);
		if (!mVU.softRsqrtCache)
			mVU.softRsqrtCache = std::make_unique<microVUSoftLowerCacheSet[]>(mVUsoftLowerCacheSize);
	}
	if (THREAD_VU1)
	{
		DevCon.Warning("mVU Reset");
		// If MTVU is toggled on during gameplay we need to flush the running VU1 program, else it gets in a mess
		if (VU0.VI[REG_VPU_STAT].UL & 0x100)
		{
			CpuVU1->Execute(vu1RunCycles);
		}
		VU0.VI[REG_VPU_STAT].UL &= ~0x100;
	}

	xSetTextPtr(mVU.textPtr());
	xSetPtr(mVU.cache);
	mVUdispatcherAB(mVU);
	mVUdispatcherCD(mVU);
	mVUGenerateWaitMTVU(mVU);
	mVUGenerateCopyPipelineState(mVU);
	mVUGenerateCompareState(mVU);
	if (use_soft_float)
	{
		mVUGenerateSoftAddExactLaneKernel(mVU);
		mVUGenerateSoftAddLaneRepairKernel(mVU);
		mVUGenerateSoftMulExactKernel(mVU);
		mVUGenerateSoftMulExactVectorKernel(mVU);
		mVUGenerateSoftMulBoothPackedKernel(mVU);
		if (use_soft_madd_packed)
			mVUGenerateSoftMaddPackedKernels(mVU);
		mVUGenerateSoftMaddIntegratedLaneKernel(mVU);
		mVUGenerateSoftMaddExactVectorKernels(mVU);
		mVUGenerateLowerSrtReciprocalSoftExactKernel(mVU);
		const mVUSoftDivCapTailPatch div_cap_tail = mVUGenerateLowerDivSoftExactKernel(mVU);
		mVUGenerateLowerSqrtSoftExactKernel(mVU);
		mVUGenerateLowerRsqrtSoftExactKernel(mVU);
		u8* const main_region_end = xGetPtr();
		u8* const rec_end = mVU.index ? SysMemory::GetVU1RecEnd() : SysMemory::GetVU0RecEnd();
		u8* const tail_begin = rec_end - mVUsoftDivCapTailReserve;
		xSetPtr(tail_begin);
		mVUGenerateLowerDivSoftCapTail(div_cap_tail);
		pxAssert(static_cast<size_t>(xGetPtr() - tail_begin) <= mVUsoftDivCapTailReserve);
		xSetPtr(main_region_end);
	}
	mVU.regs().nextBlockCycles = 0;
	memset(&mVU.prog.lpState, 0, sizeof(mVU.prog.lpState));
	mVU.profiler.Reset(mVU.index);

	// Program Variables
	mVU.prog.cleared  =  1;
	mVU.prog.isSame   = -1;
	mVU.prog.cur      = NULL;
	mVU.prog.total    =  0;
	mVU.prog.curFrame =  0;

	// Setup Dynarec Cache Limits for Each Program
	mVU.prog.x86start = xGetAlignedCallTarget();
	mVU.prog.x86ptr   = mVU.prog.x86start;

	for (u32 i = 0; i < (mVU.progSize / 2); i++)
	{
		if (!mVU.prog.prog[i])
		{
			mVU.prog.prog[i] = new std::deque<microProgram*>();
			continue;
		}
		for (auto it = mVU.prog.prog[i]->begin(); it != mVU.prog.prog[i]->end(); ++it)
		{
			mVUdeleteProg(mVU, it[0]);
		}
		mVU.prog.prog[i]->clear();
		mVU.prog.quick[i].block = NULL;
		mVU.prog.quick[i].prog = NULL;
	}
}

// Free Allocated Resources
void mVUclose(microVU& mVU)
{
	g_mvuCommunicationHint[mVU.index] = {};
	g_mvuPreparedEntry[mVU.index] = nullptr;
	// Delete Programs and Block Managers
	for (u32 i = 0; i < (mVU.progSize / 2); i++)
	{
		if (!mVU.prog.prog[i])
			continue;
		for (auto it = mVU.prog.prog[i]->begin(); it != mVU.prog.prog[i]->end(); ++it)
		{
			mVUdeleteProg(mVU, it[0]);
		}
		safe_delete(mVU.prog.prog[i]);
	}
}

// Clears Block Data in specified range
__fi void mVUclear(mV, u32 addr, u32 size)
{
	g_mvuCommunicationHint[mVU.index] = {};
	g_mvuPreparedEntry[mVU.index] = nullptr;
	if (!mVU.prog.cleared)
	{
		mVU.prog.cleared = 1; // Next execution searches/creates a new microprogram
		std::memset(&mVU.prog.lpState, 0, sizeof(mVU.prog.lpState)); // Clear pipeline state
		for (u32 i = 0; i < (mVU.progSize / 2); i++)
		{
			mVU.prog.quick[i].block = NULL; // Clear current quick-reference block
			mVU.prog.quick[i].prog = NULL; // Clear current quick-reference prog
		}
	}
}

//------------------------------------------------------------------
// Micro VU - Private Functions
//------------------------------------------------------------------

// Deletes a program
__ri void mVUdeleteProg(microVU& mVU, microProgram*& prog)
{
	g_mvuCommunicationHint[mVU.index] = {};
	g_mvuPreparedEntry[mVU.index] = nullptr;
	for (u32 i = 0; i < (mVU.progSize / 2); i++)
	{
		safe_delete(prog->block[i]);
	}
	safe_delete(prog->ranges);
	safe_aligned_free(prog);
}

// Creates a new Micro Program
__ri microProgram* mVUcreateProg(microVU& mVU, int startPC)
{
	microProgram* prog = (microProgram*)_aligned_malloc(sizeof(microProgram), 64);
	memset(prog, 0, sizeof(microProgram));
	prog->idx = mVU.prog.total++;
	prog->ranges = new std::deque<microRange>();
	prog->startPC = startPC;
	if(doWholeProgCompare)
		mVUcacheProg(mVU, *prog); // Cache Micro Program
	double cacheSize = (double)((uptr)mVU.prog.x86end - (uptr)mVU.prog.x86start);
	double cacheUsed = ((double)((uptr)mVU.prog.x86ptr - (uptr)mVU.prog.x86start)) / (double)_1mb;
	double cachePerc = ((double)((uptr)mVU.prog.x86ptr - (uptr)mVU.prog.x86start)) / cacheSize * 100;
	ConsoleColors c = mVU.index ? Color_Orange : Color_Magenta;
	DevCon.WriteLn(c, "microVU%d: Cached Prog = [%03d] [PC=%04x] [List=%02d] (Cache=%3.3f%%) [%3.1fmb]",
		mVU.index, prog->idx, startPC * 8, mVU.prog.prog[startPC]->size() + 1, cachePerc, cacheUsed);
	return prog;
}

// Caches Micro Program
__ri void mVUcacheProg(microVU& mVU, microProgram& prog)
{
	if (!doWholeProgCompare)
	{
		auto cmpOffset = [&](void* x) { return (u8*)x + mVUrange.start; };
		memcpy(cmpOffset(prog.data), cmpOffset(mVU.regs().Micro), (mVUrange.end - mVUrange.start));
	}
	else
	{
		if (!mVU.index)
			memcpy(prog.data, mVU.regs().Micro, 0x1000);
		else
			memcpy(prog.data, mVU.regs().Micro, 0x4000);
	}
	mVUdumpProg(mVU, prog);
}

// Generate Hash for partial program based on compiled ranges...
u64 mVUrangesHash(microVU& mVU, microProgram& prog)
{
	union
	{
		u64 v64;
		u32 v32[2];
	} hash = {0};

	std::deque<microRange>::const_iterator it(prog.ranges->begin());
	for (; it != prog.ranges->end(); ++it)
	{
		if ((it[0].start < 0) || (it[0].end < 0))
		{
			DevCon.Error("microVU%d: Negative Range![%d][%d]", mVU.index, it[0].start, it[0].end);
		}
		for (int i = it[0].start / 4; i < it[0].end / 4; i++)
		{
			hash.v32[0] -= prog.data[i];
			hash.v32[1] ^= prog.data[i];
		}
	}
	return hash.v64;
}

// Prints the ratio of unique programs to total programs
void mVUprintUniqueRatio(microVU& mVU)
{
	std::vector<u64> v;
	for (u32 pc = 0; pc < mProgSize / 2; pc++)
	{
		microProgramList* list = mVU.prog.prog[pc];
		if (!list)
			continue;
		for (auto it = list->begin(); it != list->end(); ++it)
		{
			v.push_back(mVUrangesHash(mVU, *it[0]));
		}
	}
	u32 total = v.size();
	sortVector(v);
	makeUnique(v);
	if (!total)
		return;
	DevCon.WriteLn("%d / %d [%3.1f%%]", v.size(), total, 100. - (double)v.size() / (double)total * 100.);
}

// Compare Cached microProgram to mVU.regs().Micro
__fi bool mVUcmpProg(microVU& mVU, microProgram& prog)
{
	if (doWholeProgCompare)
	{
		if (memcmp((u8*)prog.data, mVU.regs().Micro, mVU.microMemSize))
			return false;
	}
	else
	{
		for (const auto& range : *prog.ranges)
		{
#if defined(PCSX2_DEVBUILD) || defined(_DEBUG)
			if ((range.start < 0) || (range.end < 0))
				DevCon.Error("microVU%d: Negative Range![%d][%d]", mVU.index, range.start, range.end);
#endif
			auto cmpOffset = [&](void* x) { return (u8*)x + range.start; };

			if (memcmp(cmpOffset(prog.data), cmpOffset(mVU.regs().Micro), (range.end - range.start)))
				return false;
		}
	}
	mVU.prog.cleared = 0;
	mVU.prog.cur = &prog;
	mVU.prog.isSame = doWholeProgCompare ? 1 : -1;
	return true;
}

// Searches for Cached Micro Program and sets prog.cur to it (returns entry-point to program)
_mVUt __fi void* mVUsearchProg(u32 startPC, uptr pState)
{
	microVU& mVU = mVUx;
	microProgramQuick& quick = mVU.prog.quick[mVU.regs().start_pc / 8];
	microProgramList*  list  = mVU.prog.prog [mVU.regs().start_pc / 8];

	if (!quick.prog) // If null, we need to search for new program
	{
		for (auto it = list->begin(); it != list->end(); ++it)
		{
			bool b = mVUcmpProg(mVU, *it[0]);

			if (b)
			{
				quick.block = it[0]->block[startPC / 8];
				quick.prog  = it[0];
				list->erase(it);
				list->push_front(quick.prog);

				// Sanity check, in case for some reason the program compilation aborted half way through (JALR for example)
				if (quick.block == nullptr)
				{
					void* entryPoint = mVUblockFetch(mVU, startPC, pState);
					return entryPoint;
				}
				return mVUentryGet(mVU, quick.block, startPC, pState);
			}
		}

		// If cleared and program not found, make a new program instance
		mVU.prog.cleared = 0;
		mVU.prog.isSame  = 1;
		mVU.prog.cur     = mVUcreateProg(mVU, mVU.regs().start_pc/8);
		void* entryPoint = mVUblockFetch(mVU,  startPC, pState);
		quick.block      = mVU.prog.cur->block[startPC/8];
		quick.prog       = mVU.prog.cur;
		list->push_front(mVU.prog.cur);
		//mVUprintUniqueRatio(mVU);
		return entryPoint;
	}

	// If list.quick, then we've already found and recompiled the program ;)
	mVU.prog.isSame = -1;
	mVU.prog.cur = quick.prog;
	// Because the VU's can now run in sections and not whole programs at once
	// we need to set the current block so it gets the right program back
	quick.block = mVU.prog.cur->block[startPC / 8];

	// Sanity check, in case for some reason the program compilation aborted half way through
	if (quick.block == nullptr)
	{
		void* entryPoint = mVUblockFetch(mVU, startPC, pState);
		return entryPoint;
	}
	return mVUentryGet(mVU, quick.block, startPC, pState);
}

//------------------------------------------------------------------
// recMicroVU0 / recMicroVU1
//------------------------------------------------------------------

recMicroVU0 CpuMicroVU0;
recMicroVU1 CpuMicroVU1;

recMicroVU0::recMicroVU0() { m_Idx = 0; IsInterpreter = false; }
recMicroVU1::recMicroVU1() { m_Idx = 1; IsInterpreter = false; }

void recMicroVU0::Reserve()
{
	mVUinit(microVU0, 0);
}
void recMicroVU1::Reserve()
{
	mVUinit(microVU1, 1);
	vu1Thread.Open();
}

void recMicroVU0::Shutdown()
{
	mVUclose(microVU0);
}
void recMicroVU1::Shutdown()
{
	if (vu1Thread.IsOpen())
		vu1Thread.WaitVU();
	mVUclose(microVU1);
}

void recMicroVU0::Reset()
{
	mVUreset(microVU0);
}

void recMicroVU0::Step()
{
}

void recMicroVU1::Reset()
{
	vu1Thread.WaitVU();
	vu1Thread.Get_MTVUChanges();
	mVUreset(microVU1);
}

__fi static bool mVUequalPipeline(const microRegInfo& a, const microRegInfo& b)
{
	static_assert(sizeof(microRegInfo) % sizeof(__m128i) == 0);
	const auto* lhs = reinterpret_cast<const __m128i*>(&a);
	const auto* rhs = reinterpret_cast<const __m128i*>(&b);
	__m128i diff = _mm_xor_si128(_mm_loadu_si128(lhs), _mm_loadu_si128(rhs));
	if (_mm_movemask_epi8(_mm_cmpeq_epi8(diff, _mm_setzero_si128())) != 0xffff)
		return false;
	for (size_t i = 1; i < sizeof(microRegInfo) / sizeof(__m128i); i++)
		diff = _mm_or_si128(diff, _mm_xor_si128(_mm_loadu_si128(lhs + i), _mm_loadu_si128(rhs + i)));
	return _mm_movemask_epi8(_mm_cmpeq_epi8(diff, _mm_setzero_si128())) == 0xffff;
}

static void* mVUprepareCommunication()
{
	auto& r = g_mvuCommunicationRequest;
	const VURegs& requested = vuRegs[r.requestor];
	const u32 stat = VU0.VI[REG_VPU_STAT].UL;
	if (!(stat & (r.requestor ? 0x100 : 1)) || requested.cycle >= r.target ||
		(!r.requestor && (VU0.flags & VUFLAG_MFLAGSET)))
		return nullptr;
	const u32 unit = VUCommunication::SelectUnit(stat & 1, stat & 0x100, VU0.cycle, VU1.cycle);
	VURegs& vu = vuRegs[unit];
	microVU& mvu = unit ? microVU1 : microVU0;
	const u32 pc = (vu.VI[REG_TPC].UL << 3) & (mvu.microMemSize - 8);
	auto& hint = g_mvuCommunicationHint[unit];
	const microBlock* block = nullptr;
	void* code;
	if (hint.block && hint.pc == pc && !mvu.prog.cleared &&
		hint.program == mvu.prog.cur && hint.program == mvu.prog.quick[vu.start_pc / 8].prog &&
		mVUequalPipeline(mvu.prog.lpState, hint.block->pState))
	{
		block = hint.block;
		code = block->x86ptrStart;
		xSetTextPtr(mvu.textPtr());
		xSetPtr(mvu.prog.x86ptr);
		mvu.prog.isSame = -1;
	}
	else
	{
		const microRegInfo entry = mvu.prog.lpState;
		code = unit ? mVUexecute<1>(pc, 0) : mVUexecute<0>(pc, 0);
		mvu.prog.x86ptr = xGetPtr();
		mvu.prog.lpState = entry;
		block = mvu.prog.cur->block[pc / 8]->search(mvu, &mvu.prog.lpState);
	}
	hint = {};
	pxAssert(block && block->cycles);
	r.unit = unit;
	r.pc = pc;
	r.before = vu.cycle;
	r.runCycles = block->cycles;
	g_mvuPreparedEntry[unit] = code;
	if (!unit)
		VU0.flags &= ~VUFLAG_MFLAGSET;
	vu.VI[REG_TPC].UL = pc;
	return g_mvuCommunicationBody[unit];
}

void* mVUcommunicationNext(u32 unit)
{
	VURegs& vu = vuRegs[unit];
	pxAssert(g_mvuCommunicationRequest.active && g_mvuCommunicationRequest.unit == unit &&
			 vu.cycle > g_mvuCommunicationRequest.before);
	pxAssert(!g_mvuPreparedEntry[unit]);
	vu.VI[REG_TPC].UL >>= 3;
	if (vu.flags & VUFLAG_INTCINTERRUPT)
	{
		vu.flags &= ~VUFLAG_INTCINTERRUPT;
		hwIntcIrq(unit ? 7 : 6);
	}
	return mVUprepareCommunication();
}

bool mVUrunCommunication(u32 unit, u32 cycles)
{
	if (!EmuConfig.Gamefixes.VUCommunicationHack ||
		!EmuConfig.Cpu.Recompiler.EnableVU0 || !EmuConfig.Cpu.Recompiler.EnableVU1)
		return false;
	pxAssert(!THREAD_VU1 && !g_mvuCommunicationRequest.active);
	auto& r = g_mvuCommunicationRequest;
	r = {};
	r.active = 1;
	r.requestor = unit;
	r.target = vuRegs[unit].cycle + cycles;
	if (!unit)
		VU0.flags &= ~VUFLAG_MFLAGSET;
	if (mVUprepareCommunication())
	{
		microVU& mvu = r.unit ? microVU1 : microVU0;
		((mVUrecCall)mvu.startFunct)(r.pc, r.runCycles);
	}
	r.active = 0;
	VU0.nextBlockCycles = static_cast<s32>(VU0.cycle - cpuRegs.cycle) + 1;
	VU1.nextBlockCycles = static_cast<s32>(VU1.cycle - cpuRegs.cycle) + 1;
	return true;
}

static void mVUexecuteQuantum(microVU& mVU)
{
	VURegs& vu = mVU.regs();
	const u32 pc = (vu.VI[REG_TPC].UL << 3) & (mVU.microMemSize - 8);
	const microRegInfo entry = mVU.prog.lpState;
	// Lookup can compile and temporarily change lpState. Restore the input
	// pipeline before entering the ordinary dispatcher with the measured cost.
	void* code = mVU.index ? mVUexecute<1>(pc, 0) : mVUexecute<0>(pc, 0);
	mVU.prog.x86ptr = xGetPtr();
	mVU.prog.lpState = entry;
	const microBlock* block = mVU.prog.cur->block[pc / 8]->search(mVU, &mVU.prog.lpState);
	pxAssert(block && block->cycles);
	const u32 cost = block->cycles;
	// Consumed exactly once by the dispatcher. No guest execution or cache
	// invalidation is allowed between this lookup and entry.
	g_mvuPreparedEntry[mVU.index] = code;
	if (!mVU.index)
		vu.flags &= ~VUFLAG_MFLAGSET;
	vu.VI[REG_TPC].UL = pc;
	((mVUrecCall)mVU.startFunct)(pc, cost);
	vu.VI[REG_TPC].UL >>= 3;
	pxAssert(!g_mvuPreparedEntry[mVU.index]);
	if (vu.flags & VUFLAG_INTCINTERRUPT)
	{
		vu.flags &= ~VUFLAG_INTCINTERRUPT;
		hwIntcIrq(mVU.index ? 7 : 6);
	}
}

void recMicroVU0::ExecuteQuantum()
{
	mVUexecuteQuantum(microVU0);
}

void recMicroVU1::ExecuteQuantum()
{
	mVUexecuteQuantum(microVU1);
}

void recMicroVU0::SetStartPC(u32 startPC)
{
	VU0.start_pc = startPC;
}

void recMicroVU0::Execute(u32 cycles)
{
	if (vuRunInterleaved(0, cycles))
		return;
	VU0.flags &= ~VUFLAG_MFLAGSET;

	if (!(VU0.VI[REG_VPU_STAT].UL & 1))
		return;
	VU0.VI[REG_TPC].UL <<= 3;

	((mVUrecCall)microVU0.startFunct)(VU0.VI[REG_TPC].UL, cycles);
	VU0.VI[REG_TPC].UL >>= 3;
	if (microVU0.regs().flags & 0x4)
	{
		microVU0.regs().flags &= ~0x4;
		hwIntcIrq(6);
	}
}

void recMicroVU1::SetStartPC(u32 startPC)
{
	VU1.start_pc = startPC;
}

void recMicroVU1::Step()
{
}

void recMicroVU1::Execute(u32 cycles)
{
	if (vuRunInterleaved(1, cycles))
		return;
	if (!THREAD_VU1)
	{
		if (!(VU0.VI[REG_VPU_STAT].UL & 0x100))
			return;
	}
	VU1.VI[REG_TPC].UL <<= 3;
	((mVUrecCall)microVU1.startFunct)(VU1.VI[REG_TPC].UL, cycles);
	VU1.VI[REG_TPC].UL >>= 3;
	if (microVU1.regs().flags & 0x4 && !THREAD_VU1)
	{
		microVU1.regs().flags &= ~0x4;
		hwIntcIrq(7);
	}
}

void recMicroVU0::Clear(u32 addr, u32 size)
{
	mVUclear(microVU0, addr, size);
}
void recMicroVU1::Clear(u32 addr, u32 size)
{
	mVUclear(microVU1, addr, size);
}

void recMicroVU1::ResumeXGkick()
{
	if (!(VU0.VI[REG_VPU_STAT].UL & 0x100))
		return;
	((mVUrecCallXG)microVU1.startFunctXG)();
}

bool SaveStateBase::vuJITFreeze()
{
	if (IsSaving())
		vu1Thread.WaitVU();

	Freeze(microVU0.prog.lpState);
	Freeze(microVU1.prog.lpState);
	return IsOkay();
}
