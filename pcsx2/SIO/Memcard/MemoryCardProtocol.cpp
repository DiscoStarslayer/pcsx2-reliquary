// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "SIO/Memcard/MemoryCardProtocol.h"

#include "SIO/Sio.h"
#include "SIO/Sio2.h"
#include "SIO/Sio0.h"
#include "des.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include <array>
#include <cstring>

#define MC_LOG_ENABLE 0
#define MC_LOG if (MC_LOG_ENABLE) DevCon

#define PS1_FAIL() if (this->PS1Fail()) return;

MemoryCardProtocol g_MemoryCardProtocol;

namespace
{
	using Bytes8 = std::array<u8, 8>;
	using Bytes16 = std::array<u8, 16>;

	// keysource and key are self generated values
	static constexpr Bytes8 keysource = {0xf5, 0x80, 0x95, 0x3c, 0x4c, 0x84, 0xa9, 0xc0};
	static constexpr Bytes8 coh_keysource = {0x03, 0x14, 0x93, 0x16, 0x27, 0x02, 0x9D, 0xA2};

	static constexpr Bytes16 cex_key = {0x06, 0x46, 0x7a, 0x6c, 0x5b, 0x9b, 0x82, 0x77, 0x0d, 0xdf, 0xe9, 0x7e, 0x24, 0x5b, 0x9f, 0xca}; // SCPH-10020 in Retail mode
	static constexpr Bytes16 dex_key = {0x17, 0x39, 0xD3, 0xBC, 0xD0, 0x2C, 0x18, 0x07, 0x0F, 0x7A, 0xF3, 0xB7, 0x9E, 0x73, 0x03, 0x1C}; // SCPH-10020 in Developer mode or SCPH-10020T
	static constexpr Bytes16 coh_key = {0x05, 0x3D, 0x59, 0x77, 0xC4, 0xF7, 0xB0, 0xD4, 0x37, 0xAE, 0x66, 0xA5, 0x17, 0x71, 0xB8, 0xC0}; // COH-H10020
	static constexpr Bytes16 coh_cex_key = {0xA9, 0xFB, 0x27, 0x2A, 0x63, 0xCF, 0xED, 0x6F, 0xD0, 0x28, 0xA2, 0x4A, 0x98, 0x11, 0xB8, 0x2E}; // SCPH-10020 in Arcade mode
	static constexpr Bytes16 prt_key = {0x8C, 0x4B, 0xEF, 0xA6, 0xF4, 0x9A, 0x23, 0xA0, 0x9C, 0xF1, 0x46, 0xAA, 0x17, 0x1C, 0xFE, 0x75}; // Prototype Memory Card (EB-10020?)

	struct MemoryCardAuthState
	{
		const u8* key = cex_key.data();
		Bytes8 iv = {};
		Bytes8 seed = {};
		Bytes8 nonce = {};
		Bytes8 mechaChallenge1 = {};
		Bytes8 mechaChallenge2 = {};
		Bytes8 mechaChallenge3 = {};
		Bytes8 mechaResponse1 = {};
		Bytes8 mechaResponse2 = {};
		Bytes8 mechaResponse3 = {};
		std::array<u8, 9> cryptBuf = {};
		u8 cryptXorResult = 0;
	};

	static std::array<MemoryCardAuthState, 8> s_auth_state;

	static u32 getActiveMemoryCardSlot()
	{
		if (!mcd)
			return 0;

		const u32 slot = sioConvertPortAndSlotToPad(mcd->port, mcd->slot);
		return (slot < s_auth_state.size()) ? slot : 0;
	}

	static MemoryCardAuthState& getActiveAuthState(u32 slot)
	{
		return s_auth_state[slot];
	}

	static const Bytes8& getConfiguredKeySource(u32 slot)
	{
		switch (EmuConfig.Mcd[slot].KeySource)
		{
			case MemoryCardKeySource::Arcade:
				return coh_keysource;
			case MemoryCardKeySource::Retail:
			case MemoryCardKeySource::MaxCount:
			default:
				return keysource;
		}
	}

	static const u8* getConfiguredKey(u32 slot)
	{
		switch (EmuConfig.Mcd[slot].Key)
		{
			case MemoryCardKey::Development:
				return dex_key.data();
			case MemoryCardKey::Arcade:
				return coh_key.data();
			case MemoryCardKey::Conquest:
				return coh_cex_key.data();
			case MemoryCardKey::Prototype:
				return prt_key.data();
			case MemoryCardKey::Retail:
			case MemoryCardKey::MaxCount:
			default:
				return cex_key.data();
		}
	}
}

static void desEncrypt(const void* key, void* data)
{
	DesContext dc;
	desInit(&dc, static_cast<const uint8_t*>(key), 8);
	desEncryptBlock(&dc, (uint8_t*)data, (uint8_t*)data);
}

static void desDecrypt(const void* key, void* data)
{
	DesContext dc;
	desInit(&dc, static_cast<const uint8_t*>(key), 8);
	desDecryptBlock(&dc, (uint8_t*)data, (uint8_t*)data);
}

static void doubleDesEncrypt(const void* key, void* data)
{
	const u8* keyBytes = static_cast<const u8*>(key);
	desEncrypt(keyBytes, data);
	desDecrypt(&keyBytes[8], data);
	desEncrypt(keyBytes, data);
}

static void doubleDesDecrypt(const void* key, void* data)
{
	const u8* keyBytes = static_cast<const u8*>(key);
	desDecrypt(keyBytes, data);
	desEncrypt(&keyBytes[8], data);
	desDecrypt(keyBytes, data);
}

static void xor_bit(const void* a, const void* b, void* Result, size_t Length)
{
	size_t i;
	for (i = 0; i < Length; i++)
	{
		((uint8_t*)Result)[i] = ((uint8_t*)a)[i] ^ ((uint8_t*)b)[i];
	}
}

static void generateIvSeedNonce(u32 slot, MemoryCardAuthState& auth)
{
	const Bytes8& source = getConfiguredKeySource(slot);
	for (int i = 0; i < 8; i++)
	{
		auth.iv[i] = static_cast<u8>(rand());
		auth.seed[i] = source[i] ^ auth.iv[i];
		auth.nonce[i] = static_cast<u8>(rand());
	}
}

static void generateResponse(MemoryCardAuthState& auth)
{
	uint8_t challengeIV[8] = {/* SHA256: e7b02f4f8d99a58b96dbca4db81c5d666ea7c46fbf6e1d5c045eaba0ee25416a */};
	if (!EmuConfig.Security.MgChallengeIvFile.empty())
	{
		Error error;
		std::string path = Path::Canonicalize(EmuConfig.Security.MgChallengeIvFile);
		auto fp = FileSystem::OpenManagedCFileTryIgnoreCase(path.c_str(), "rb", &error);
		if (!fp || std::fread(challengeIV, 1, sizeof(challengeIV), fp.get()) == 0)
		{
			ERROR_LOG("Failed to read Challenge IV file at {}: {}", path, error.GetDescription());
		}
	}

	doubleDesDecrypt(auth.key, auth.mechaChallenge1.data());
	Bytes8 random;
	xor_bit(auth.mechaChallenge1.data(), challengeIV, random.data(), random.size());

	// MechaChallenge2 and MechaChallenge3 lets the card verify the console

	xor_bit(auth.nonce.data(), challengeIV, auth.mechaResponse1.data(), auth.mechaResponse1.size());
	doubleDesEncrypt(auth.key, auth.mechaResponse1.data());

	xor_bit(random.data(), auth.mechaResponse1.data(), auth.mechaResponse2.data(), auth.mechaResponse2.size());
	doubleDesEncrypt(auth.key, auth.mechaResponse2.data());

	static constexpr Bytes8 cardKey = {'M', 'e', 'c', 'h', 'a', 'P', 'w', 'n'};
	xor_bit(cardKey.data(), auth.mechaResponse2.data(), auth.mechaResponse3.data(), auth.mechaResponse3.size());
	doubleDesEncrypt(auth.key, auth.mechaResponse3.data());
}

// Check if the memcard is for PS1, and if we are working on a command sent over SIO2.
// If so, return dead air.
bool MemoryCardProtocol::PS1Fail()
{
	if (mcd->IsPSX() && g_Sio2.commandLength > 0)
	{
		while (g_Sio2FifoOut.size() < g_Sio2.commandLength)
		{
			g_Sio2FifoOut.push_back(0x00);
		}

		return true;
	}

	return false;
}

// A repeated pattern in memcard commands is to pad with zero bytes,
// then end with 0x2b and terminator bytes. This function is a shortcut for that.
void MemoryCardProtocol::The2bTerminator(size_t length)
{
	while (g_Sio2FifoOut.size() < length - 2)
	{
		g_Sio2FifoOut.push_back(0x00);
	}

	g_Sio2FifoOut.push_back(0x2b);
	g_Sio2FifoOut.push_back(mcd->term);
}

// After one read or write, the memcard is almost certainly going to be issued a new read or write
// for the next segment of the same sector. Bump the transferAddr to where that segment begins.
// If it is the end and a new sector is being accessed, the SetSector function will deal with
// both sectorAddr and transferAddr.
void MemoryCardProtocol::ReadWriteIncrement(size_t length)
{
	mcd->transferAddr += length;
}

void MemoryCardProtocol::RecalculatePS1Addr()
{
	mcd->sectorAddr = ((ps1McState.sectorAddrMSB << 8) | ps1McState.sectorAddrLSB);
	mcd->goodSector = (mcd->sectorAddr <= 0x03ff);
	mcd->transferAddr = 128 * mcd->sectorAddr;
}

void MemoryCardProtocol::ResetPS1State()
{
	ps1McState.currentByte = 2;
	ps1McState.sectorAddrMSB = 0;
	ps1McState.sectorAddrLSB = 0;
	ps1McState.checksum = 0;
	ps1McState.expectedChecksum = 0;
	memset(ps1McState.buf.data(), 0, ps1McState.buf.size());
}

void MemoryCardProtocol::Probe()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();

	if (!mcd->IsPresent())
	{
		g_Sio2FifoOut.push_back(0xff);
		g_Sio2FifoOut.push_back(0xff);
		g_Sio2FifoOut.push_back(0xff);
		g_Sio2FifoOut.push_back(0xff);
	}
	else
	{
		The2bTerminator(4);
	}
}

void MemoryCardProtocol::UnknownWriteDeleteEnd()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	The2bTerminator(4);
}

void MemoryCardProtocol::SetSector()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	const u8 sectorLSB = g_Sio2FifoIn.front();
	g_Sio2FifoIn.pop_front();
	const u8 sector2nd = g_Sio2FifoIn.front();
	g_Sio2FifoIn.pop_front();
	const u8 sector3rd = g_Sio2FifoIn.front();
	g_Sio2FifoIn.pop_front();
	const u8 sectorMSB = g_Sio2FifoIn.front();
	g_Sio2FifoIn.pop_front();
	const u8 expectedChecksum = g_Sio2FifoIn.front();
	g_Sio2FifoIn.pop_front();

	u8 computedChecksum = sectorLSB ^ sector2nd ^ sector3rd ^ sectorMSB;
	mcd->goodSector = (computedChecksum == expectedChecksum);

	if (!mcd->goodSector)
	{
		Console.Warning("%s() Warning! Memcard sector checksum failed! (Expected %02X != Actual %02X) Please report to the PCSX2 team!", __FUNCTION__, expectedChecksum, computedChecksum);
	}

	u32 newSector = sectorLSB | (sector2nd << 8) | (sector3rd << 16) | (sectorMSB << 24);
	mcd->sectorAddr = newSector;

	McdSizeInfo info;
	mcd->GetSizeInfo(info);
	mcd->transferAddr = (info.SectorSize + 16) * mcd->sectorAddr;

	The2bTerminator(9);
}

void MemoryCardProtocol::GetSpecs()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	//u8 checksum = 0x00;
	McdSizeInfo info;
	mcd->GetSizeInfo(info);
	g_Sio2FifoOut.push_back(0x2b);
	
	const u8 sectorSizeLSB = (info.SectorSize & 0xff);
	//checksum ^= sectorSizeLSB;
	g_Sio2FifoOut.push_back(sectorSizeLSB);

	const u8 sectorSizeMSB = (info.SectorSize >> 8);
	//checksum ^= sectorSizeMSB;
	g_Sio2FifoOut.push_back(sectorSizeMSB);

	const u8 eraseBlockSizeLSB = (info.EraseBlockSizeInSectors & 0xff);
	//checksum ^= eraseBlockSizeLSB;
	g_Sio2FifoOut.push_back(eraseBlockSizeLSB);

	const u8 eraseBlockSizeMSB = (info.EraseBlockSizeInSectors >> 8);
	//checksum ^= eraseBlockSizeMSB;
	g_Sio2FifoOut.push_back(eraseBlockSizeMSB);

	const u8 sectorCountLSB = (info.McdSizeInSectors & 0xff);
	//checksum ^= sectorCountLSB;
	g_Sio2FifoOut.push_back(sectorCountLSB);

	const u8 sectorCount2nd = (info.McdSizeInSectors >> 8);
	//checksum ^= sectorCount2nd;
	g_Sio2FifoOut.push_back(sectorCount2nd);

	const u8 sectorCount3rd = (info.McdSizeInSectors >> 16);
	//checksum ^= sectorCount3rd;
	g_Sio2FifoOut.push_back(sectorCount3rd);

	const u8 sectorCountMSB = (info.McdSizeInSectors >> 24);
	//checksum ^= sectorCountMSB;
	g_Sio2FifoOut.push_back(sectorCountMSB);
	
	g_Sio2FifoOut.push_back(info.Xor);
	g_Sio2FifoOut.push_back(mcd->term);
}

void MemoryCardProtocol::SetTerminator()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	mcd->term = g_Sio2FifoIn.front();
	g_Sio2FifoIn.pop_front();
	g_Sio2FifoOut.push_back(0x00);
	g_Sio2FifoOut.push_back(0x2b);
	g_Sio2FifoOut.push_back(mcd->term);
}

// This one is a bit unusual. Old and new versions of MCMAN seem to handle this differently.
// Some commands may check [4] for the terminator. Others may check [3]. Typically, older
// MCMAN revisions will exclusively check [4], and newer revisions will check both [3] and [4]
// for different values. In all cases, they expect to see a valid terminator value.
//
// Also worth noting old revisions of MCMAN will not set anything other than 0x55 for the terminator,
// while newer revisions will set the terminator to another value (most commonly 0x5a).
void MemoryCardProtocol::GetTerminator()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	g_Sio2FifoOut.push_back(0x2b);
	g_Sio2FifoOut.push_back(mcd->term);
	g_Sio2FifoOut.push_back(mcd->term);
}

void MemoryCardProtocol::WriteData()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	g_Sio2FifoOut.push_back(0x00);
	g_Sio2FifoOut.push_back(0x2b);
	const u8 writeLength = g_Sio2FifoIn.front();
	g_Sio2FifoIn.pop_front();
	u8 checksum = 0x00;
	std::vector<u8> buf;

	for (size_t writeCounter = 0; writeCounter < writeLength; writeCounter++)
	{
		const u8 writeByte = g_Sio2FifoIn.front();
		g_Sio2FifoIn.pop_front();
		checksum ^= writeByte;
		buf.push_back(writeByte);
		g_Sio2FifoOut.push_back(0x00);
	}

	mcd->Write(buf.data(), buf.size());
	g_Sio2FifoOut.push_back(checksum);
	g_Sio2FifoOut.push_back(mcd->term);

	ReadWriteIncrement(writeLength);

	MemcardBusy::SetBusy();
}

void MemoryCardProtocol::ReadData()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	const u8 readLength = g_Sio2FifoIn.front();
	g_Sio2FifoIn.pop_front();
	g_Sio2FifoOut.push_back(0x00);
	g_Sio2FifoOut.push_back(0x2b);
	std::vector<u8> buf;
	buf.resize(readLength);
	mcd->Read(buf.data(), buf.size());
	u8 checksum = 0x00;

	for (const u8 readByte : buf)
	{
		checksum ^= readByte;
		g_Sio2FifoOut.push_back(readByte);
	}

	g_Sio2FifoOut.push_back(checksum);
	g_Sio2FifoOut.push_back(mcd->term);

	ReadWriteIncrement(readLength);
}

u8 MemoryCardProtocol::PS1Read(u8 data)
{
	MC_LOG.WriteLn("%s", __FUNCTION__);

	if (!mcd->IsPresent())
	{
		return 0xff;
	}

	bool sendAck = true;
	u8 ret = 0;

	switch (ps1McState.currentByte)
	{
		case 2:
			ret = 0x5a;
			break;
		case 3:
			ret = 0x5d;
			break;
		case 4:
			ps1McState.sectorAddrMSB = data;
			ret = 0x00;
			break;
		case 5:
			ps1McState.sectorAddrLSB = data;
			ret = 0x00;
			RecalculatePS1Addr();
			break;
		case 6:
			ret = 0x5c;
			break;
		case 7:
			ret = 0x5d;
			break;
		case 8:
			ret = ps1McState.sectorAddrMSB;
			break;
		case 9:
			ret = ps1McState.sectorAddrLSB;
			break;
		case 138:
			ret = ps1McState.checksum;
			break;
		case 139:
			ret = 0x47;
			sendAck = false;
			break;
		case 10:
			ps1McState.checksum = ps1McState.sectorAddrMSB ^ ps1McState.sectorAddrLSB;
			mcd->Read(ps1McState.buf.data(), ps1McState.buf.size());
			[[fallthrough]];
		default:
			ret = ps1McState.buf[ps1McState.currentByte - 10];
			ps1McState.checksum ^= ret;
			break;
	}

	g_Sio0.SetAcknowledge(sendAck);

	ps1McState.currentByte++;
	return ret;
}

u8 MemoryCardProtocol::PS1State(u8 data)
{
	Console.Error("%s(%02X) I do not exist, please change that ASAP.", __FUNCTION__, data);
	pxFail("Missing PS1State handler");
	return 0x00;
}

u8 MemoryCardProtocol::PS1Write(u8 data)
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	bool sendAck = true;
	u8 ret = 0;

	switch (ps1McState.currentByte)
	{
		case 2:
			ret = 0x5a;
			break;
		case 3:
			ret = 0x5d;
			break;
		case 4:
			ps1McState.sectorAddrMSB = data;
			ret = 0x00;
			break;
		case 5:
			ps1McState.sectorAddrLSB = data;
			ret = 0x00;
			RecalculatePS1Addr();
			break;
		case 134:
			ps1McState.expectedChecksum = data;
			ret = 0;
			break;
		case 135:
			ret = 0x5c;
			break;
		case 136:
			ret = 0x5d;
			break;
		case 137:
			if (!mcd->goodSector)
			{
				ret = 0xff;
			}
			else if (ps1McState.expectedChecksum != ps1McState.checksum)
			{
				ret = 0x4e;
			}
			else
			{
				mcd->Write(ps1McState.buf.data(), ps1McState.buf.size());
				ret = 0x47;
				// Clear the "directory unread" bit of the flag byte. Per no$psx, this is cleared
				// on writes, not reads.
				mcd->FLAG &= 0x07;
			}

			sendAck = false;
			break;
		case 6:
			ps1McState.checksum = ps1McState.sectorAddrMSB ^ ps1McState.sectorAddrLSB;
			[[fallthrough]];
		default:
			ps1McState.buf[ps1McState.currentByte - 6] = data;
			ps1McState.checksum ^= data;
			ret = 0x00;
			break;
	}

	g_Sio0.SetAcknowledge(sendAck);
	ps1McState.currentByte++;

	MemcardBusy::SetBusy();
	return ret;
}

u8 MemoryCardProtocol::PS1Pocketstation(u8 data)
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	g_Sio0.SetAcknowledge(false);
	return 0x00;
}

void MemoryCardProtocol::ReadWriteEnd()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	The2bTerminator(4);
}

void MemoryCardProtocol::EraseBlock()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	mcd->EraseBlock();
	The2bTerminator(4);

	MemcardBusy::SetBusy();
}

void MemoryCardProtocol::UnknownBoot()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	The2bTerminator(5);
}

void MemoryCardProtocol::AuthXor()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	const u32 slot = getActiveMemoryCardSlot();
	MemoryCardAuthState& auth = getActiveAuthState(slot);
	const u8 modeByte = g_Sio2FifoIn.front();
	g_Sio2FifoIn.pop_front();

	switch (modeByte)
	{
		case 0x01: // get iv
		{
			generateIvSeedNonce(slot, auth);
			g_Sio2FifoOut.push_back(0x00);
			g_Sio2FifoOut.push_back(0x2b);
			u8 xorResult = 0x00;

			for (size_t xorCounter = 0; xorCounter < 8; xorCounter++)
			{
				const u8 toXOR = auth.iv[7 - xorCounter];
				g_Sio2FifoIn.pop_front();
				xorResult ^= toXOR;
				g_Sio2FifoOut.push_back(toXOR);
			}
			g_Sio2FifoOut.push_back(xorResult);
			g_Sio2FifoOut.push_back(mcd->term);
			break;
		}
		case 0x02: // get seed
		{
			g_Sio2FifoOut.push_back(0x00);
			g_Sio2FifoOut.push_back(0x2b);
			u8 xorResult = 0x00;
			for (size_t xorCounter = 0; xorCounter < 8; xorCounter++)
			{
				const u8 toXOR = auth.seed[7 - xorCounter];
				g_Sio2FifoIn.pop_front();
				xorResult ^= toXOR;
				g_Sio2FifoOut.push_back(toXOR);
			}
			g_Sio2FifoOut.push_back(xorResult);
			g_Sio2FifoOut.push_back(mcd->term);
			break;
		}
		case 0x04: // get nonce
		{
			g_Sio2FifoOut.push_back(0x00);
			g_Sio2FifoOut.push_back(0x2b);
			u8 xorResult = 0x00;
			for (size_t xorCounter = 0; xorCounter < 8; xorCounter++)
			{
				const u8 toXOR = auth.nonce[7 - xorCounter];
				g_Sio2FifoIn.pop_front();
				xorResult ^= toXOR;
				g_Sio2FifoOut.push_back(toXOR);
			}
			g_Sio2FifoOut.push_back(xorResult);
			g_Sio2FifoOut.push_back(mcd->term);
			break;
		}
		case 0x06:
		{
			for (size_t i = 0; i < 8; i++)
			{
				const u8 val = g_Sio2FifoIn.front();
				g_Sio2FifoIn.pop_front();
				auth.mechaChallenge3[7 - i] = val;
			}
			The2bTerminator(14);
			break;
		}
		case 0x07:
		{
			for (size_t i = 0; i < 8; i++)
			{
				const u8 val = g_Sio2FifoIn.front();
				g_Sio2FifoIn.pop_front();
				auth.mechaChallenge2[7 - i] = val;
			}
			The2bTerminator(14);
			break;
		}
		case 0x0b:
		{
			for (size_t i = 0; i < 8; i++)
			{
				const u8 val = g_Sio2FifoIn.front();
				g_Sio2FifoIn.pop_front();
				auth.mechaChallenge1[7 - i] = val;
			}
			The2bTerminator(14);
			break;
		}
		case 0x0f: // CardResponse1
		{
			generateResponse(auth);
			g_Sio2FifoOut.push_back(0x00);
			g_Sio2FifoOut.push_back(0x2b);
			u8 xorResult = 0x00;
			for (size_t xorCounter = 0; xorCounter < 8; xorCounter++)
			{
				const u8 toXOR = auth.mechaResponse1[7 - xorCounter];
				g_Sio2FifoIn.pop_front();
				xorResult ^= toXOR;
				g_Sio2FifoOut.push_back(toXOR);
			}
			g_Sio2FifoOut.push_back(xorResult);
			g_Sio2FifoOut.push_back(mcd->term);
			break;
		}
		case 0x11: // CardResponse2
		{
			g_Sio2FifoOut.push_back(0x00);
			g_Sio2FifoOut.push_back(0x2b);
			u8 xorResult = 0x00;
			for (size_t xorCounter = 0; xorCounter < 8; xorCounter++)
			{
				const u8 toXOR = auth.mechaResponse2[7 - xorCounter];
				g_Sio2FifoIn.pop_front();
				xorResult ^= toXOR;
				g_Sio2FifoOut.push_back(toXOR);
			}
			g_Sio2FifoOut.push_back(xorResult);
			g_Sio2FifoOut.push_back(mcd->term);
			break;
		}
		case 0x13: // CardResponse3
		{
			g_Sio2FifoOut.push_back(0x00);
			g_Sio2FifoOut.push_back(0x2b);
			u8 xorResult = 0x00;

			for (size_t xorCounter = 0; xorCounter < 8; xorCounter++)
			{
				const u8 toXOR = auth.mechaResponse3[7 - xorCounter];
				g_Sio2FifoIn.pop_front();
				xorResult ^= toXOR;
				g_Sio2FifoOut.push_back(toXOR);
			}

			g_Sio2FifoOut.push_back(xorResult);
			g_Sio2FifoOut.push_back(mcd->term);
			break;
		}
		case 0x00:
		case 0x03:
		case 0x05:
		case 0x08:
		case 0x09:
		case 0x0a:
		case 0x0c:
		case 0x0d:
		case 0x0e:
		case 0x10:
		case 0x12:
		case 0x14:
		{
			The2bTerminator(5);
			break;
		}
		default:
			Console.Warning("%s(queue) Unexpected modeByte (%02X), please report to the PCSX2 team", __FUNCTION__, modeByte);
			break;
	}
}

void MemoryCardProtocol::AuthCrypt()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	MemoryCardAuthState& auth = getActiveAuthState(getActiveMemoryCardSlot());
	const u8 modeByte = g_Sio2FifoIn.front();
	g_Sio2FifoIn.pop_front();

	switch (modeByte)
	{
		case 0x40:
		case 0x50:
		case 0x42:
		case 0x52:
			The2bTerminator(5);
			break;
		case 0x41:
		case 0x51:
			auth.cryptXorResult = 0;
			for (size_t i = 0; i < 8; i++)
			{
				const u8 val = g_Sio2FifoIn.front();
				g_Sio2FifoIn.pop_front();
				auth.cryptXorResult ^= val;
				auth.cryptBuf[i] = val;
			}
			The2bTerminator(14);
			break;
		case 0x43:
		case 0x53:
			g_Sio2FifoOut.push_back(0x00);
			g_Sio2FifoOut.push_back(0x2b);
			for (size_t i = 0; i < 8; i++)
			{
				g_Sio2FifoOut.push_back(auth.cryptBuf[i]);
			}
			g_Sio2FifoOut.push_back(auth.cryptXorResult);
			g_Sio2FifoOut.push_back(mcd->term);
			break;
		default:
			Console.Warning("%s(queue) Unexpected modeByte (%02X), please report to the PCSX2 team", __FUNCTION__, modeByte);
			break;
	}
}

void MemoryCardProtocol::AuthReset()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();

	if (!mcd->IsPresent())
	{
		g_Sio2FifoOut.push_back(0xff);
		g_Sio2FifoOut.push_back(0xff);
		g_Sio2FifoOut.push_back(0xff);
		g_Sio2FifoOut.push_back(0xff);
	}
	else
	{
		mcd->term = Terminator::READY;
		const u32 slot = getActiveMemoryCardSlot();
		getActiveAuthState(slot).key = getConfiguredKey(slot);
		The2bTerminator(5);
	}
}

void MemoryCardProtocol::AuthKeySelect()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	const u8 data = g_Sio2FifoIn.front();
	g_Sio2FifoIn.pop_front();
	if (data == 1)
	{
		getActiveAuthState(getActiveMemoryCardSlot()).key = cex_key.data();
	}
	The2bTerminator(5);
}
