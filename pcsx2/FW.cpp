// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "IopDma.h"
#include "R3000A.h"
#include "FW.h"

#include "common/Console.h"

#include <cstdlib>
#include <cstring>

static u8 phyregs[16];
s8* fwregs;


void logControl0Regs(u32 value)
{
	bool rcv_self_id = (value & 0x8000'0000) >> 0x1f;
	bool sidf = (value & 0x4000'0000) >> 0x1e;
	uint8_t de_lim = (value & 0x3000'0000) >> 0x1c;
	bool tx_en = (value & 0x0800'0000) >> 0x1b;
	bool rx_en = (value & 0x0400'0000) >> 0x1a;
	bool tx_res = (value & 0x0200'0000) >> 0x19;
	bool rx_rst = (value & 0x0100'0000) >> 0x18;
	bool bus_id_rst = (value & 0x0080'0000) >> 0x17;
	bool c_mstr = (value & 0x0040'0000) >> 0x16;
	bool cyc_tmr_en = (value & 0x0020'0000) >> 0x15;
	bool ext_cyc = (value & 0x0010'0000) >> 0x14;
	bool root = (value & 0x0008'0000) >> 0x13;
	bool brde = (value & 0x0004'0000) >> 0x12;
	bool s_tardy = (value & 0x0002'0000) >> 0x11;
	bool loose_tight_iso = (value & 0x0001'0000) >> 0x10;
	uint8_t ret_lim = (value & 0x0000'f000) >> 0xc;
	uint16_t pri_lim = (value & 0x0000'0fc0) >> 0x6;
	bool rsp_0 = (value & 0x0000'0020) >> 0x5;
	bool u_rcv_m = (value & 0x0000'0010) >> 0x4;
	uint8_t ctrl_0_res = (value & 0x0000'000f);

	DevCon.WriteLn("##: Dumping control 0 regs ############");
	DevCon.WriteLn("##: Receive Self ID: 0x%x", rcv_self_id);
	DevCon.WriteLn("##: Self ID Format: 0x%x", sidf);
	DevCon.WriteLn("##: Data Error Retry Limit: 0x%x", de_lim);
	DevCon.WriteLn("##: Transmitter Enable: 0x%x", tx_en);
	DevCon.WriteLn("##: Receiver Enable: 0x%x", rx_en);
	DevCon.WriteLn("##: Transmitter Reset: 0x%x", tx_res);
	DevCon.WriteLn("##: Receiver Reset: 0x%x", rx_rst);
	DevCon.WriteLn("##: Bus ID Reset: 0x%x", bus_id_rst);
	DevCon.WriteLn("##: Cycle Master: 0x%x", c_mstr);
	DevCon.WriteLn("##: Cycle Timer Enable: 0x%x", cyc_tmr_en);
	DevCon.WriteLn("##: External Cycle: 0x%x", ext_cyc);
	DevCon.WriteLn("##: Root: 0x%x", root);
	DevCon.WriteLn("##: Busy Received Data Errors: 0x%x", brde);
	DevCon.WriteLn("##: Send Tardy: 0x%x", s_tardy);
	DevCon.WriteLn("##: Loose ISO Cycles: 0x%x", loose_tight_iso);
	DevCon.WriteLn("##: Retry Limit: 0x%x", ret_lim);
	DevCon.WriteLn("##: Priority Request Limit: 0x%x", pri_lim);
	DevCon.WriteLn("##: Route SelfID Packets to PHT0: 0x%x", rsp_0);
	DevCon.WriteLn("##: UBuf Receive Multiple Packets: 0x%x", u_rcv_m);
	DevCon.WriteLn("##: Reserved: 0x%x", ctrl_0_res);
	DevCon.WriteLn("#######################################");
}

void logPhyRegs()
{
	u32 value = PHYACC;
	uint8_t addr = (value & 0x0f00'0000u) >> 24;

	uint8_t physical_id;
	bool r;
	bool ps;

	bool rhb;
	bool ibr;
	uint8_t gap_count;

	uint8_t extended;
	uint8_t total_ports;

	uint8_t max_speed;
	uint8_t delay;

	bool l_ctrl;
	bool contender;
	uint8_t jitter;
	uint8_t pwr_class;

	bool watchdog;
	bool isbr;
	bool loop;
	bool pwr_fail;
	bool timeout;
	bool port_event;
	bool enab_accel;
	bool enab_multi;

	// 0x06 reg reserved

	uint8_t page_select;
	uint8_t port_select;

	// 0x08 page select properties
	// page_select = 0x0 Port Status
	uint8_t a_stat;
	uint8_t b_stat;
	bool child;
	bool connected;
	bool bias;
	bool disabled;

	uint8_t negotiated_speed;
	bool int_enable;
	bool fault;

	// page_select = 0x1 Vendor ID page
	uint8_t compliance_level;

	// 0x09 reserved

	uint8_t vendor_id_h;

	uint8_t vendor_id_m;

	uint8_t vendor_id_l;

	uint8_t product_id_h;

	uint8_t product_id_m;

	uint8_t product_id_l;

	// page_select = 0x7 Vendor Dependent page
	uint8_t pzadj;
	bool ext_dp_s100;
	bool enable_sr;

	uint8_t phy_reg = phyregs[addr];

	switch (addr)
	{
		case 0x00:
			physical_id = (phy_reg >> 2) & 0x1f;
			r = (phy_reg >> 1) & 0x1;
			ps = phy_reg & 0x1;
			DevCon.WriteLn("##: Physical ID: 0x%x", physical_id);
			DevCon.WriteLn("##: Root: 0x%x", r);
			DevCon.WriteLn("##: Power Status: 0x%x", ps);
			break;
		case 0x01:
			rhb = (phy_reg >> 7) & 0x1;
			ibr = (phy_reg >> 6) & 0x1;
			gap_count = phy_reg & 0x3f;
			DevCon.WriteLn("##: Root Hold-Off Bit: 0x%x", rhb);
			DevCon.WriteLn("##: Initiate Bus Reset 0x%x", ibr);
			DevCon.WriteLn("##: Gap Count: 0x%x", gap_count);
			break;
		case 0x02:
			extended = (phy_reg >> 5) & 0x7;
			total_ports = phy_reg & 0x1f;
			DevCon.WriteLn("##: Extended 0x%x", extended);
			DevCon.WriteLn("##: Total Ports: 0x%x", total_ports);
			break;
		case 0x03:
			max_speed = (phy_reg >> 5) & 0x7;
			delay = phy_reg & 0xf;
			DevCon.WriteLn("##: Max Speed: 0x%x", max_speed);
			DevCon.WriteLn("##: Delay: 0x%x", delay);
			break;
		case 0x04:
			l_ctrl = (phy_reg >> 7) & 0x1;
			contender = (phy_reg >> 6) & 0x1;
			jitter = (phy_reg >> 3) & 0x7;
			pwr_class = phy_reg & 0x7;
			DevCon.WriteLn("##: Link Active Report Control: 0x%x", l_ctrl);
			DevCon.WriteLn("##: Contender: 0x%x", contender);
			DevCon.WriteLn("##: Jitter: 0x%x", jitter);
			DevCon.WriteLn("##: Power Class: 0x%x", pwr_class);
			break;
		case 0x05:
			watchdog = (phy_reg >> 7) & 0x1;
			isbr = (phy_reg >> 6) & 0x1;
			loop = (phy_reg >> 5) & 0x1;
			pwr_fail = (phy_reg >> 4) & 0x1;
			timeout = (phy_reg >> 3) & 0x1;
			port_event = (phy_reg >> 2) & 0x1;
			enab_accel = (phy_reg >> 1) & 0x1;
			enab_multi = phy_reg & 0x1;
			DevCon.WriteLn("##: Watchdog: 0x%x", watchdog);
			DevCon.WriteLn("##: Initiate Short Bus Reset: 0x%x", isbr);
			DevCon.WriteLn("##: Loop Detect: 0x%x", loop);
			DevCon.WriteLn("##: Cable Power Failure Detect: 0x%x", pwr_fail);
			DevCon.WriteLn("##: Timeout Detect: 0x%x", timeout);
			DevCon.WriteLn("##: Port Event Detect: 0x%x", port_event);
			DevCon.WriteLn("##: Enable Arbitration Acceleration: 0x%x", enab_accel);
			DevCon.WriteLn("##: Enable Multispeed Packet Concatenation: 0x%x", enab_multi);
			break;
		case 0x06:
			DevCon.WriteLn("##: Wrote to reserved reg");
			break;
		case 0x07:
			page_select = (phy_reg >> 5) & 0x7;
			port_select = phy_reg & 0xf;
			DevCon.WriteLn("##: Page Select: 0x%x", page_select);
			DevCon.WriteLn("##: Port Select: 0x%x", port_select);
			break;
		case 0x08:
			page_select = (phyregs[0x07] >> 5) & 0x7;
			port_select = phyregs[0x07] & 0xf;
			DevCon.WriteLn("##: Page Select: 0x%x", page_select);
			DevCon.WriteLn("##: Port Select: 0x%x", port_select);
			if (page_select == 0)
			{
				a_stat = (phy_reg >> 6) & 0x3;
				b_stat = (phy_reg >> 4) & 0x3;
				child = (phy_reg >> 3) & 0x1;
				connected = (phy_reg >> 2) & 0x1;
				bias = (phy_reg >> 1) & 0x1;
				disabled = (phy_reg >> 0) & 0x1;
				DevCon.WriteLn("##: TPA Line State: 0x%x", a_stat);
				DevCon.WriteLn("##: TPB Line State: 0x%x", b_stat);
				DevCon.WriteLn("##: Child: 0x%x", child);
				DevCon.WriteLn("##: Connected: 0x%x", connected);
				DevCon.WriteLn("##: Bias Voltage: 0x%x", bias);
				DevCon.WriteLn("##: Port Disabled: 0x%x", disabled);
			}
			if (page_select == 1)
			{
				compliance_level = phy_reg;
				DevCon.WriteLn("##: Compliance Level: 0x%x", compliance_level);
			}
			if (page_select == 7)
			{
				pzadj = (phy_reg >> 4) & 0xf;
				ext_dp_s100 = (phy_reg >> 1) & 0x1;
				enable_sr = phy_reg & 0x1;
				DevCon.WriteLn("##: PLL Adjust: 0x%x", pzadj);
				DevCon.WriteLn("##: Extend Data Prefix for S100 Packets: 0x%x", ext_dp_s100);
				DevCon.WriteLn("##: Enable Suspend Resume: 0x%x", enable_sr);
			}
			break;
		case 0x09:
			page_select = (phyregs[0x07] >> 5) & 0x7;
			port_select = phyregs[0x07] & 0xf;
			DevCon.WriteLn("##: Page Select: 0x%x", page_select);
			DevCon.WriteLn("##: Port Select: 0x%x", port_select);
			if (page_select == 0)
			{
				negotiated_speed = (phy_reg >> 5) & 0x7;
				int_enable = (phy_reg >> 4) & 0x1;
				fault = (phy_reg >> 3) & 0x1;
				DevCon.WriteLn("##: Maximum Speed Negotiated: 0x%x", negotiated_speed);
				DevCon.WriteLn("##: Enable Port Event Interrupts: 0x%x", int_enable);
				DevCon.WriteLn("##: Fault: 0x%x", fault);
			}
			if (page_select == 1)
			{
				DevCon.WriteLn("## Reserved");
			}
			if (page_select == 7)
			{
				DevCon.WriteLn("## Reserved");
			}
			break;
		case 0x0a:
			page_select = (phyregs[0x07] >> 5) & 0x7;
			port_select = phyregs[0x07] & 0xf;
			DevCon.WriteLn("##: Page Select: 0x%x", page_select);
			DevCon.WriteLn("##: Port Select: 0x%x", port_select);
			if (page_select == 0)
			{
				DevCon.WriteLn("## Reserved");
			}
			if (page_select == 1)
			{
				vendor_id_h = phy_reg;
				DevCon.WriteLn("##: Vendor ID: 0x%x", vendor_id_h);
			}
			if (page_select == 7)
			{
				DevCon.WriteLn("## Reserved");
			}
			break;
		case 0x0b:
			page_select = (phyregs[0x07] >> 5) & 0x7;
			port_select = phyregs[0x07] & 0xf;
			DevCon.WriteLn("##: Page Select: 0x%x", page_select);
			DevCon.WriteLn("##: Port Select: 0x%x", port_select);
			if (page_select == 0)
			{
				DevCon.WriteLn("## Reserved");
			}
			if (page_select == 1)
			{
				vendor_id_m = phy_reg;
				DevCon.WriteLn("##: Vendor ID: 0x%x", vendor_id_m);
			}
			if (page_select == 7)
			{
				DevCon.WriteLn("## Reserved");
			}
			break;
		case 0x0c:
			page_select = (phyregs[0x07] >> 5) & 0x7;
			port_select = phyregs[0x07] & 0xf;
			DevCon.WriteLn("##: Page Select: 0x%x", page_select);
			DevCon.WriteLn("##: Port Select: 0x%x", port_select);
			if (page_select == 0)
			{
				DevCon.WriteLn("## Reserved");
			}
			if (page_select == 1)
			{
				vendor_id_l = phy_reg;
				DevCon.WriteLn("##: Vendor ID: 0x%x", vendor_id_l);
			}
			if (page_select == 7)
			{
				DevCon.WriteLn("## Reserved");
			}
			break;
		case 0x0d:
			page_select = (phyregs[0x07] >> 5) & 0x7;
			port_select = phyregs[0x07] & 0xf;
			DevCon.WriteLn("##: Page Select: 0x%x", page_select);
			DevCon.WriteLn("##: Port Select: 0x%x", port_select);
			if (page_select == 0)
			{
				DevCon.WriteLn("## Reserved");
			}
			if (page_select == 1)
			{
				product_id_h = phy_reg;
				DevCon.WriteLn("##: Product ID: 0x%x", product_id_h);
			}
			if (page_select == 7)
			{
				DevCon.WriteLn("## Reserved");
			}
			break;
		case 0x0e:
			page_select = (phyregs[0x07] >> 5) & 0x7;
			port_select = phyregs[0x07] & 0xf;
			DevCon.WriteLn("##: Page Select: 0x%x", page_select);
			DevCon.WriteLn("##: Port Select: 0x%x", port_select);
			if (page_select == 0)
			{
				DevCon.WriteLn("## Reserved");
			}
			if (page_select == 1)
			{
				product_id_m = phy_reg;
				DevCon.WriteLn("##: Product ID: 0x%x", product_id_m);
			}
			if (page_select == 7)
			{
				DevCon.WriteLn("## Reserved");
			}
			break;
		case 0x0f:
			page_select = (phyregs[0x07] >> 5) & 0x7;
			port_select = phyregs[0x07] & 0xf;
			DevCon.WriteLn("##: Page Select: 0x%x", page_select);
			DevCon.WriteLn("##: Port Select: 0x%x", port_select);
			if (page_select == 0)
			{
				DevCon.WriteLn("## Reserved");
			}
			if (page_select == 1)
			{
				product_id_l = phy_reg;
				DevCon.WriteLn("##: Product ID: 0x%x", product_id_l);
			}
			if (page_select == 7)
			{
				DevCon.WriteLn("## Reserved");
			}
			break;

	}
}

void logPhyAccessRegs()
{
	u32 value = PHYACC;
	bool rd_phy = (value & 0x8000'0000u) > 0;
	bool wr_phy = (value & 0x4000'0000u) > 0;
	uint8_t reserved_0 = (value & 0x3000'0000u) >> 28;
	uint8_t phy_reg_addr = (value & 0x0f00'0000u) >> 24;
	uint8_t phy_rg_dat = (value & 0x00ff'0000u) >> 16;
	uint8_t reserved_1 = (value & 0x0000'f000u) >> 12;
	uint8_t phy_rx_adr = (value & 0x0000'0f00u) >> 8;
	uint8_t phy_rx_dat = (value & 0x0000'00ffu);

	DevCon.WriteLn("##: Dumping PHY regs ############");
	DevCon.WriteLn("##: Read PHY: 0x%x", rd_phy);
	DevCon.WriteLn("##: Write PHY: 0x%x", wr_phy);
	DevCon.WriteLn("##: Phy Reg Addr: 0x%x", phy_reg_addr);
	DevCon.WriteLn("##: Phy Reg Data: 0x%x", phy_rg_dat);
	DevCon.WriteLn("##: Phy Receive Addr: 0x%x", phy_rx_adr);
	DevCon.WriteLn("##: Phy Receive Data: 0x%x", phy_rx_dat);
	DevCon.WriteLn("#################################");
}

void logFwAction(u32 addr, u32 value, bool write)
{
	const char* mode;
	const char* writeS = "write";
	const char* readS = "read";
	if (write != 0)
	{
		mode = writeS;
	}
	else
	{
		mode = readS;
	}

	switch (addr)
	{
		case 0x1f808400:
			DevCon.WriteLn("FW: %s node id 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808404:
			DevCon.WriteLn("FW: %s cycle time 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808408:
			DevCon.WriteLn("FW: %s ctrl 0 0x%x: 0x%x", mode, addr, value);
			logControl0Regs(value);
			break;
		case 0x1f80840c:
			DevCon.WriteLn("FW: %s ctrl 1 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808410:
			DevCon.WriteLn("FW: %s ctrl 2 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808414:
			DevCon.WriteLn("FW: %s PHY Access 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808418:
		case 0x1f80841c:
			DevCon.WriteLn("FW: %s Unknown Register 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808420:
			DevCon.WriteLn("FW: %s interrupt 0 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808424:
			DevCon.WriteLn("FW: %s interrupt mask 0 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808428:
			DevCon.WriteLn("FW: %s interrupt 1 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f80842c:
			DevCon.WriteLn("FW: %s interrupt mask 1 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808430:
			DevCon.WriteLn("FW: %s interrupt 2 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808434:
			DevCon.WriteLn("FW: %s interrupt mask 2 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808438:
			DevCon.WriteLn("FW: %s dmar 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f80843c:
			DevCon.WriteLn("FW: %s ack status 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808440:
			DevCon.WriteLn("FW: %s ubuf transmit next 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808444:
			DevCon.WriteLn("FW: %s ubuf transmit last 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808448:
			DevCon.WriteLn("FW: %s ubuf transmit clear 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f80844c:
			DevCon.WriteLn("FW: %s ubuf receive clear 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808450:
			DevCon.WriteLn("FW: %s ubuf receive 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808454:
			DevCon.WriteLn("FW: %s ubuf receive level 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808458:
		case 0x1f80845c:
		case 0x1f808460:
		case 0x1f808464:
		case 0x1f808468:
		case 0x1f80846c:
			DevCon.WriteLn("FW: %s unmapped 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808470:
		case 0x1f808474:
		case 0x1f808478:
			DevCon.WriteLn("FW: %s unknown register 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f80847c:
			DevCon.WriteLn("FW: %s power management register 0x%x: 0x%x", mode, addr, value);
			// iLinkman sets this to 0x40 then 0x0 after delay to shutdown the link and phy
			// Konami loader sets this to 0x41 on boot, likely to do the same?
			// Then sets value to 0x1
			break;
		case 0x1f808480:
			DevCon.WriteLn("FW: %s PHT ctrl st r0 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808484:
			DevCon.WriteLn("FW: %s PHT split to r0 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808488:
			DevCon.WriteLn("FW: %s PHT req res hdr0 r0 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f80848c:
			DevCon.WriteLn("FW: %s PHT req res hdr1 r0 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808490:
			DevCon.WriteLn("FW: %s PHT req res hdr2 r0 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808494:
			DevCon.WriteLn("FW: %s STR x NID Sel0 r0 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808498:
			DevCon.WriteLn("FW: %s STR x NID Sel1 r0 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f80849c:
			DevCon.WriteLn("FW: %s STR x HDR r0 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f8084a0:
			DevCon.WriteLn("FW: %s STT x HDR r0 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f8084a4:
			DevCon.WriteLn("FW: %s DTrans CTRL 0 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f8084a8:
			DevCon.WriteLn("FW: %s CIP Hdr Tx 0 r0 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f8084ac:
			DevCon.WriteLn("FW: %s CIP Hdr Tx 1 r0 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f8084b0:
			DevCon.WriteLn("FW: %s padding 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f8084b4:
			DevCon.WriteLn("FW: %s STT x time stamp offset r0 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f8084b8:
			DevCon.WriteLn("FW: %s dma ctrl SR0 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f8084bc:
			DevCon.WriteLn("FW: %s dma trans TRSH0 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f8084c0:
			DevCon.WriteLn("FW: %s dbuf FIFO lvl r0 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f8084c4:
			DevCon.WriteLn("FW: %s dbuf Tx data r0 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f8084c8:
			DevCon.WriteLn("FW: %s dbuf Rx data r0 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f8084cc:
			DevCon.WriteLn("FW: %s dbuf watermarks r0 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f8084d0:
			DevCon.WriteLn("FW: %s dbuf FIFO size r0 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f8084d4:
		case 0x1f8084d8:
		case 0x1f8084dc:
		case 0x1f8084e0:
		case 0x1f8084e4:
		case 0x1f8084e8:
		case 0x1f8084ec:
		case 0x1f8084f0:
		case 0x1f8084f4:
		case 0x1f8084f8:
		case 0x1f8084fc:
			DevCon.WriteLn("FW: %s unmapped 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808500:
			DevCon.WriteLn("FW: %s PHT ctrl st r1 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808504:
			DevCon.WriteLn("FW: %s PHT split to r1 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808508:
			DevCon.WriteLn("FW: %s PHT req res hdr0 r1 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f80850c:
			DevCon.WriteLn("FW: %s PHT req res hdr1 r1 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808510:
			DevCon.WriteLn("FW: %s PHT req res hdr2 r1 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808514:
			DevCon.WriteLn("FW: %s STR x NID Sel0 r1 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808518:
			DevCon.WriteLn("FW: %s STR x NID Sel1 r1 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f80851c:
			DevCon.WriteLn("FW: %s STR x HDR r1 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808520:
			DevCon.WriteLn("FW: %s STT x HDR r1 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808524:
			DevCon.WriteLn("FW: %s DTrans CTRL 1 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808528:
			DevCon.WriteLn("FW: %s CIP Hdr Tx 0 r1 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f80852c:
			DevCon.WriteLn("FW: %s CIP Hdr Tx 1 r1 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808530:
			DevCon.WriteLn("FW: %s padding 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808534:
			DevCon.WriteLn("FW: %s STT x time stamp offset r1 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808538:
			DevCon.WriteLn("FW: %s dma ctrl SR1 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f80853c:
			DevCon.WriteLn("FW: %s dma trans TRSH1 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808540:
			DevCon.WriteLn("FW: %s dbuf FIFO lvl r1 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808544:
			DevCon.WriteLn("FW: %s dbuf Tx data r1 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808548:
			DevCon.WriteLn("FW: %s dbuf Rx data r1 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f80854c:
			DevCon.WriteLn("FW: %s dbuf watermarks r1 0x%x: 0x%x", mode, addr, value);
			break;
		case 0x1f808550:
			DevCon.WriteLn("FW: %s dbuf FIFO size r1 0x%x: 0x%x", mode, addr, value);
			break;
	}
}

s32 FWopen()
{
	memset(phyregs, 0, sizeof(phyregs));
	// Initializing our registers.
	fwregs = (s8*)calloc(0x10000, 1);
	if (fwregs == NULL)
	{
		DevCon.WriteLn("FW: Error allocating Memory");
		return -1;
	}
	return 0;
}

void FWclose()
{
	// Freeing the registers.
	free(fwregs);
	fwregs = NULL;
}

void PHYWrite()
{
	u8 reg = (PHYACC >> 24) & 0xf;
	u8 data = (PHYACC >> 16) & 0xff;

	phyregs[reg] = data;

	bool readback = (PHYACC & 0x8000'0000u) > 0;

	PHYACC &= 0xBFFF'FFFF; // Clear WrPhy bit

	if (readback)
	{
		PHYRead();
	}
}

void PHYRead()
{
	u8 reg = (PHYACC >> 24) & 0xf;

	PHYACC &= 0x7FFF'F000; // Clear RdPhy bit and RX data

	PHYACC |= phyregs[reg] | (reg << 8);

	if (fwRu32(0x8424) & 0x40000000) //RRx interrupt mask
	{
		fwRu32(0x8420) |= 0x40000000;
		fwIrq();
	}
}

u32 FWread32(u32 addr)
{
	u32 ret = 0;

	switch (addr)
	{
		//Node ID Register the top part is default, bottom part i got from my ps2
		case 0x1f808400:
			// TODO: Valid bit stuck to 0 till phy selfId task completes
			// it's incorrect to instantly self-id
			ret = /*(0x3ff << 22) | 1;*/ 0xffc00001;
			break;
		// Control Register 2
		case 0x1f808410:
			ret = fwRu32(addr); //SCLK OK (Needs to be set when FW is "Ready"
			break;
		//Interrupt 0 Register
		case 0x1f808420:
			ret = fwRu32(addr);
			break;

		//Dunno what this is, but my home console always returns this value 0x10000001
		//Seems to be related to the Node ID however (does some sort of compare/check)
		case 0x1f80847c:
			ret = 0x10000001;
			break;

		default:
			// By default, read fwregs.
			ret = fwRu32(addr);
			break;
	}

	logFwAction(addr, ret, false);
	if (addr == 0x1f808414)
	{
		logPhyAccessRegs();
		logPhyRegs();
	}
	return ret;
}

void FWwrite32(u32 addr, u32 value)
{
	logFwAction(addr, value, true);
	switch (addr)
	{
		//		Include other memory locations we want to catch here.
		//		For example:
		//
		//		case 0x1f808400:
		//		case 0x1f808414:
		//		case 0x1f808420:
		//		case 0x1f808428:
		//		case 0x1f808430:
		//

		//PHY access
		case 0x1f808414:
			//If in read mode (top bit set) we read the PHY register requested then set the RRx interrupt if it's enabled
			//Im presuming we send that back to pcsx2 then. This register stores the result, plus whatever was written (minus the read/write flag
			fwRu32(addr) = value;   //R/W Bit cleaned in underneath function
			logPhyAccessRegs();
			if (value & 0x40000000) //Writing to PHY, write can also request a readback by setting read bit so this check is done first
			{
				PHYWrite();
			}
			else if (value & 0x80000000) //Reading from PHY
			{
				PHYRead();
			}
			logPhyRegs();
			break;

		//Control Register 0
		case 0x1f808408:
			//This enables different functions of the link interface
			//Just straight writes, should brobably struct these later.
			//Default written settings (on unreal tournament) are
			//Urcv M = 1
			//RSP 0 = 1
			//Retlim = 0xF
			//Cyc Tmr En = 1
			//Bus ID Rst = 1
			//Rcv Self ID = 1
			fwRu32(addr) = value;
			//	if((value & 0x800000) && (fwRu32(0x842C) & 0x2))
			//	{
			//		fwRu32(0x8428) |= 0x2;
			//		FWirq();
			//	}
			fwRu32(addr) &= ~0x800000;
			break;
		//Control Register 2
		case 0x1f808410: // fwRu32(addr) = value; break;
			//Ignore writes to this for now, apart from 0x2 which is Link Power Enable
			//0x8 is SCLK OK (Ready) which should be set for emulation
			fwRu32(addr) = 0x8 | value & 0x2;
			break;
		//Interrupt 0 Register
		case 0x1f808420:
		//Interrupt 1 Register
		case 0x1f808428:
		//Interrupt 2 Register
		case 0x1f808430:
			//Writes to interrupt register clears the corresponding interrupt bit
			fwRu32(addr) &= ~value;
			break;
		//Interrupt 0 Register Mask
		case 0x1f808424:
		//Interrupt 1 Register Mask
		case 0x1f80842C:
		//Interrupt 2 Register Mask
		case 0x1f808434:
			//These are direct writes (as it's a mask!)
			fwRu32(addr) = value;
			break;
		//DMA Control and Status Register 0
		case 0x1f8084B8:
			fwRu32(addr) = value;
			break;
		//DMA Control and Status Register 1
		case 0x1f808538:
			fwRu32(addr) = value;
			break;
		default:
			// By default, just write it to fwregs.
			fwRu32(addr) = value;
			break;
	}
}
