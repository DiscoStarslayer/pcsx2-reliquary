// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "VUmicro.h"
#include "VUCommunication.h"
#include "MTVU.h"
#include "GS.h"
#include "Gif_Unit.h"

BaseVUmicroCPU* CpuVU0 = nullptr;
BaseVUmicroCPU* CpuVU1 = nullptr;

void BaseVUmicroCPU::ExecuteQuantum()
{
	VURegs& vu = vuRegs[m_Idx];
	const FPControlRegisterBackup fpcr(m_Idx ? EmuConfig.Cpu.VU1FPCR : EmuConfig.Cpu.VU0FPCR);
	vu.VI[REG_TPC].UL <<= 3;
	Step();
	if (!(VU0.VI[REG_VPU_STAT].UL & (m_Idx ? 0x100 : 1)) && (m_Idx ? vu.branch == 1 : vu.branch != 0))
	{
		vu.VI[REG_TPC].UL = vu.branchpc;
		vu.branch = 0;
	}
	vu.VI[REG_TPC].UL >>= 3;
}

bool vuRunInterleaved(u32 unit, u32 cycles)
{
	if (!EmuConfig.Gamefixes.VUCommunicationHack)
		return false;

	pxAssert(!THREAD_VU1);
	VURegs& requested = vuRegs[unit];
	const u64 target = requested.cycle + cycles;
	const u32 mask = unit ? 0x100 : 1;
	if (!unit)
		VU0.flags &= ~VUFLAG_MFLAGSET;

	while ((VU0.VI[REG_VPU_STAT].UL & mask) && requested.cycle < target)
	{
		const bool active0 = (VU0.VI[REG_VPU_STAT].UL & 1) != 0;
		const bool active1 = (VU0.VI[REG_VPU_STAT].UL & 0x100) != 0;
		// At a tie VU0 accesses the shared register file before VU1 admits its pair.
		const u32 next = VUCommunication::SelectUnit(active0, active1, VU0.cycle, VU1.cycle);
		[[maybe_unused]] const u64 before = vuRegs[next].cycle;
		(next ? CpuVU1 : CpuVU0)->ExecuteQuantum();
		pxAssert(vuRegs[next].cycle > before);
		if (!unit && (VU0.flags & VUFLAG_MFLAGSET))
			break;
	}
	VU0.nextBlockCycles = static_cast<s32>(VU0.cycle - cpuRegs.cycle) + 1;
	VU1.nextBlockCycles = static_cast<s32>(VU1.cycle - cpuRegs.cycle) + 1;
	return true;
}

__inline u32 CalculateMinRunCycles(u32 cycles, bool requiresAccurateCycles)
{
	if (EmuConfig.Gamefixes.VUCommunicationHack)
		return std::max(1U, cycles);
	// If we're running an interlocked COP2 operation
	// run for an exact amount of cycles
	if(requiresAccurateCycles)
		return cycles;

	// Allow a minimum of 16 cycles to avoid running small blocks
	// Running a block of like 3 cycles is highly inefficient
	// so while sync isn't tight, it's okay to run ahead a little bit.
	return std::max(16U, cycles);
}

// Executes a Block based on EE delta time
void BaseVUmicroCPU::ExecuteBlock(bool startUp)
{
	const u32& stat = VU0.VI[REG_VPU_STAT].UL;
	const int test = m_Idx ? 0x100 : 1;

	if (m_Idx && THREAD_VU1)
	{
		vu1Thread.Get_MTVUChanges();
		return;
	}

	if (!(stat & test))
	{
		// VU currently flushes XGKICK on VU1 end so no need for this, yet
		/*if (m_Idx == 1 && VU1.xgkickenable)
		{
			_vuXGKICKTransfer((cpuRegs.cycle - VU1.xgkicklastcycle), false);
		}*/
		return;
	}

	if (startUp)
	{
		Execute(CalculateMinRunCycles(0, false));
	}
	else // Continue Executing
	{
		u64 cycle = m_Idx ? VU1.cycle : VU0.cycle;
		s32 delta = (s32)(u32)(cpuRegs.cycle - cycle);

		if (delta > 0)
			Execute(CalculateMinRunCycles(delta, false));
	}
}

// This function is called by VU0 Macro (COP2) after transferring some
// EE data to VU0's registers. We want to run VU0 Micro right after this
// to ensure that the register is used at the correct time.
// This fixes spinning/hanging in some games like Ratchet and Clank's Intro.
void BaseVUmicroCPU::ExecuteBlockJIT(BaseVUmicroCPU* cpu, bool interlocked)
{
	const u32& stat = VU0.VI[REG_VPU_STAT].UL;
	constexpr int test = 1;

	if (stat & test)
	{ // VU is running
		s64 delta = (s64)(u64)(cpuRegs.cycle - VU0.cycle);

		if (delta > 0)
		{
			cpu->Execute(CalculateMinRunCycles(delta, interlocked)); // Execute the time since the last call
		}
	}
}
