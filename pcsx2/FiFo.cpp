// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#include "Gif.h"
#include "Gif_Unit.h"
#include "MTGS.h"
#include "Vif.h"
#include "Vif_Dma.h"
#include <vector>

bool vif1CpuFifoEnabled()
{
	// Finish accepted writes even if the compatibility option is changed.
	return EmuConfig.Gamefixes.VUCommunicationHack || vif1CpuFifoPending();
}

static std::vector<u32> s_vif1_cpu_words;
static bool s_vif1_cpu_draining = false;
// One store may wait on the bus outside the 16-QW device FIFO. Both EE
// backends must yield after that store, before executing another instruction.
static mem128_t s_vif1_cpu_store = {};
static bool s_vif1_cpu_store_pending = false;

bool vif1CpuFifoBusBlocked()
{
	return s_vif1_cpu_store_pending;
}

const bool* vif1CpuFifoBusBlockedAddress()
{
	return &s_vif1_cpu_store_pending;
}

bool vif1CpuFifoActive()
{
	return s_vif1_cpu_draining;
}

bool vif1CpuFifoPending()
{
	return !s_vif1_cpu_words.empty();
}

void vif1CpuFifoReset()
{
	s_vif1_cpu_words.clear();
	s_vif1_cpu_store_pending = false;
	s_vif1_cpu_store = {};
}

bool vif1CpuFifoFreeze(SaveStateBase& state)
{
	if (state.GetVersion() < 1)
	{
		if (state.IsLoading())
			vif1CpuFifoReset();
		return state.IsOkay();
	}

	u32 count = static_cast<u32>(s_vif1_cpu_words.size());
	state.Freeze(count);
	// Reject unsupported/corrupt state before allocating beyond the device FIFO.
	if (!state.IsOkay() || count > 64 || (count && !vif1CpuFifoEnabled()))
		return false;
	if (state.IsLoading())
		s_vif1_cpu_words.resize(count);
	state.FreezeMem(s_vif1_cpu_words.data(), count * sizeof(u32));
	state.Freeze(s_vif1_cpu_store_pending);
	state.Freeze(s_vif1_cpu_store);
	// A held bus write can exist only while the device FIFO has no room
	// for another QW. Otherwise a loaded state could block EE forever with
	// no queued input available to release the write.
	if (s_vif1_cpu_store_pending && (!vif1CpuFifoEnabled() || count <= 60))
		return false;
	return state.IsOkay();
}

static void vif1UpdateCpuFifoStatus()
{
	if (vif1.cmd)
	{
		if (vif1.done && !vif1ch.qwc)
			vif1Regs.stat.VPS = VPS_WAITING;
	}
	else
		vif1Regs.stat.VPS = VPS_IDLE;

	if (gifRegs.stat.APATH == 2 && gifUnit.gifPath[1].isDone())
	{
		gifRegs.stat.APATH = 0;
		gifRegs.stat.OPH = 0;
		vif1Regs.stat.VGW = false;
		if (gifUnit.checkPaths(1, 0, 1))
			gifUnit.Execute(false, true);
	}
}

void vif1CpuFifoDrain()
{
	if (!vif1CpuFifoEnabled() || s_vif1_cpu_words.empty())
		return;
	if (s_vif1_cpu_draining || vif1Regs.stat.FDR ||
		vif1Regs.stat.test(VIF1_STAT_VSS | VIF1_STAT_VIS | VIF1_STAT_VFS))
		return;
	s_vif1_cpu_draining = true;
	// Pending CPU words precede a newly started DMA. Keep its word offset
	// separate from the already compacted CPU FIFO input.
	const tVIF_CTRL dma_offset = vif1.irqoffset;
	vif1.irqoffset = {};
	// VSS/VFS/VIS above block until STC. With those flags clear, both a
	// completed timing wait and a cancelled interrupt stall may resume.
	vif1.vifstalled.enabled = false;
	gifUnit.Execute(false, true);
	const size_t before = s_vif1_cpu_words.size();
	VIF1transfer(s_vif1_cpu_words.data(), static_cast<int>(before), true);
	const size_t consumed = before - vif1.vifpacketsize;
	s_vif1_cpu_words.erase(s_vif1_cpu_words.begin(), s_vif1_cpu_words.begin() + consumed);
	vif1.irqoffset = dma_offset;
	if (s_vif1_cpu_store_pending && s_vif1_cpu_words.size() <= 60)
	{
		s_vif1_cpu_words.insert(s_vif1_cpu_words.end(), s_vif1_cpu_store._u32, s_vif1_cpu_store._u32 + 4);
		s_vif1_cpu_store_pending = false;
	}
	if (vif1.irq && !vif1.cmd)
	{
		if (!vif1Regs.stat.ER1)
			vif1Regs.stat.INT = true;
		if (((vif1Regs.code >> 24) & 0x7f) != 0x07)
			vif1Regs.stat.VIS = true;
		hwIntcIrq(VIF1intc);
		--vif1.irq;
	}
	vif1Regs.stat.FQC = std::min<u32>(16, static_cast<u32>((s_vif1_cpu_words.size() + 3) / 4));
	vif1UpdateCpuFifoStatus();
	if (!s_vif1_cpu_words.empty())
	{
		CPU_SET_DMASTALL(VIF_VU1_FINISH, true);
		CPU_INT(VIF_VU1_FINISH, 128);
	}
	s_vif1_cpu_draining = false;
}

//////////////////////////////////////////////////////////////////////////
/////////////////////////// Quick & dirty FIFO :D ////////////////////////
//////////////////////////////////////////////////////////////////////////

// Notes on FIFO implementation
//
// The FIFO consists of four separate pages of HW register memory, each mapped to a
// PS2 device.  They are listed as follows:
//
// 0x4000 - 0x5000 : VIF0  (all registers map to 0x4000)
// 0x5000 - 0x6000 : VIF1  (all registers map to 0x5000)
// 0x6000 - 0x7000 : GS    (all registers map to 0x6000)
// 0x7000 - 0x8000 : IPU   (registers map to 0x7000 and 0x7010, respectively)

void ReadFIFO_VIF1(mem128_t* out)
{
	if (vif1Regs.stat.test(VIF1_STAT_INT | VIF1_STAT_VSS | VIF1_STAT_VIS | VIF1_STAT_VFS))
		DevCon.Warning("Reading from vif1 fifo when stalled");

	ZeroQWC(out); // Clear first in case no data gets written...
	pxAssertRel(vif1Regs.stat.FQC != 0, "FQC = 0 on VIF FIFO READ!");
	if (vif1Regs.stat.FDR)
	{
		if (vif1Regs.stat.FQC > vif1.GSLastDownloadSize)
		{
			DevCon.Warning("Warning! GS Download size < FIFO count!");
		}
		if (vif1Regs.stat.FQC > 0)
		{
			MTGS::InitAndReadFIFO(reinterpret_cast<u8*>(out), 1);
			vif1.GSLastDownloadSize--;
			GUNIT_LOG("ReadFIFO_VIF1");
			if (vif1.GSLastDownloadSize <= 16)
				gifRegs.stat.OPH = false;
			vif1Regs.stat.FQC = std::min((u32)16, vif1.GSLastDownloadSize);
		}
	}

	VIF_LOG("ReadFIFO/VIF1 -> 0x%08X.%08X.%08X.%08X", out->_u32[0], out->_u32[1], out->_u32[2], out->_u32[3]);
}

//////////////////////////////////////////////////////////////////////////
// WriteFIFO Pages
//
void WriteFIFO_VIF0(const mem128_t* value)
{
	VIF_LOG("WriteFIFO/VIF0 <- 0x%08X.%08X.%08X.%08X", value->_u32[0], value->_u32[1], value->_u32[2], value->_u32[3]);

	vif0ch.qwc += 1;
	if (vif0.irqoffset.value != 0 && vif0.vifstalled.enabled)
		DevCon.Warning("Offset on VIF0 FIFO start!");
	[[maybe_unused]] bool ret = VIF0transfer((u32*)value, 4);

	if (vif0.cmd)
	{
		if (vif0.done && vif0ch.qwc == 0)
			vif0Regs.stat.VPS = VPS_WAITING;
	}
	else
	{
		vif0Regs.stat.VPS = VPS_IDLE;
	}

	pxAssertMsg(ret, "vif stall code not implemented");
}

void WriteFIFO_VIF1(const mem128_t* value)
{
	VIF_LOG("WriteFIFO/VIF1 <- 0x%08X.%08X.%08X.%08X", value->_u32[0], value->_u32[1], value->_u32[2], value->_u32[3]);
	if (vif1CpuFifoEnabled())
	{
		pxAssertRel(!s_vif1_cpu_store_pending, "EE executed another store while the VIF1 bus write was blocked");
		if (s_vif1_cpu_words.size() > 60)
		{
			s_vif1_cpu_store = *value;
			s_vif1_cpu_store_pending = true;
			CPU_SET_DMASTALL(VIF_VU1_FINISH, true);
			CPU_INT(VIF_VU1_FINISH, 128);
			cpuSetEvent();
			return;
		}
		s_vif1_cpu_words.insert(s_vif1_cpu_words.end(), value->_u32, value->_u32 + 4);
		vif1Regs.stat.FQC = std::min<u32>(16, static_cast<u32>((s_vif1_cpu_words.size() + 3) / 4));
		vif1CpuFifoDrain();
		return;
	}

	if (vif1Regs.stat.FDR)
	{
		DevCon.Warning("writing to fifo when fdr is set!");
	}
	if (vif1Regs.stat.test(VIF1_STAT_INT | VIF1_STAT_VSS | VIF1_STAT_VIS | VIF1_STAT_VFS))
	{
		DevCon.Warning("writing to vif1 fifo when stalled");
	}
	if (vif1.irqoffset.value != 0 && vif1.vifstalled.enabled)
	{
		DevCon.Warning("Offset on VIF1 FIFO start!");
	}

	[[maybe_unused]] bool ret = VIF1transfer((u32*)value, 4);
	vif1UpdateCpuFifoStatus();

	pxAssertMsg(ret, "vif stall code not implemented");
}

void WriteFIFO_GIF(const mem128_t* value)
{
	GUNIT_LOG("WriteFIFO_GIF()");
	if ((!gifUnit.CanDoPath3() || gif_fifo.fifoSize > 0))
	{
		//DevCon.Warning("GIF FIFO HW Write");
		gif_fifo.write_fifo((u32*)value, 1);
		gif_fifo.read_fifo();
	}
	else
	{
		gifUnit.TransferGSPacketData(GIF_TRANS_FIFO, (u8*)value, 16);
	}

	if (gifUnit.gifPath[GIF_PATH_3].state == GIF_PATH_WAIT)
		gifUnit.gifPath[GIF_PATH_3].state = GIF_PATH_IDLE;

	if (gifRegs.stat.APATH == 3)
	{
		gifRegs.stat.APATH = 0;
		gifRegs.stat.OPH = 0;

		if (gifUnit.gifPath[GIF_PATH_3].state == GIF_PATH_IDLE || gifUnit.gifPath[GIF_PATH_3].state == GIF_PATH_WAIT)
		{
			if (gifUnit.checkPaths(1, 1, 0))
				gifUnit.Execute(false, true);
		}
	}
}
