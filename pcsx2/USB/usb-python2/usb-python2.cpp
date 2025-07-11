#include "usb-python2.h"

#include "Host.h"
#include "StateWrapper.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/SettingsInterface.h"
#include "devices/acio.h"
#include "devices/ddr_extio.h"
#include "devices/icca.h"
#include "devices/input_device.h"
#include "USB/qemu-usb/desc.h"
#include "USB/qemu-usb/qusb.h"

namespace usb_python2
{
	static const char* APINAME = "python2";

	struct P2IO_PACKET_HEADER
	{
		uint8_t magic;
		uint8_t len;
		uint8_t seqNo;
		uint8_t cmd;
	};

	enum
	{
		// cmd_... names are from internal namings from symbols
		P2IO_CMD_GET_VERSION = 0x01, // cmd_getver
		P2IO_CMD_RESEND = 0x02, // resend_cmd
		P2IO_CMD_FWRITEMODE = 0x03, // cmd_fwritemode 0x03aa
		P2IO_CMD_SET_WATCHDOG = 0x05, // cmd_watchdog
		P2IO_CMD_SET_AV_MASK = 0x22, // cmd_avmask
		P2IO_CMD_GET_AV_REPORT = 0x23, // cmd_avreport
		P2IO_CMD_LAMP_OUT = 0x24, // cmd_lampout = 0x24, cmd_out_all = 0x24ff
		P2IO_CMD_DALLAS = 0x25, // cmd_dallas
		P2IO_CMD_SEND_IR = 0x26, // cmd_irsend
		P2IO_CMD_READ_DIPSWITCH = 0x27, // cmd_dipsw
		P2IO_CMD_GET_JAMMA_POR = 0x28, // cmd_jamma_por
		P2IO_CMD_PORT_READ = 0x29, // cmd_portread
		P2IO_CMD_PORT_READ_POR = 0x2a, // cmd_portread_por
		P2IO_CMD_JAMMA_START = 0x2f, // cmd_jammastart
		P2IO_CMD_COIN_STOCK = 0x31, // cmd_coinstock
		P2IO_CMD_COIN_COUNTER = 0x32, // cmd_coincounter
		P2IO_CMD_COIN_BLOCKER = 0x33, // cmd_coinblocker
		P2IO_CMD_COIN_COUNTER_OUT = 0x34, // cmd_coincounterout
		P2IO_CMD_SCI_SETUP = 0x38, // cmd_scisetup
		P2IO_CMD_SCI_WRITE = 0x3a, // cmd_sciwrite
		P2IO_CMD_SCI_READ = 0x3b // cmd_sciread
	};

	enum
	{
		P2IO_AVREPORT_MODE_15KHZ = 0x80, // DDR says it's 15kHz, bootloader says 16kHz
		P2IO_AVREPORT_MODE_31KHZ = 0,
	};

	enum
	{
		GAMETYPE_DM = 0,
		GAMETYPE_GF,
		GAMETYPE_DDR,
		GAMETYPE_TOYSMARCH,
		GAMETYPE_THRILLDRIVE,
		GAMETYPE_DANCE864
	};

	enum
	{
		GN845PWBB_STAGE_IDLE = 0,
		GN845PWBB_STAGE_INIT,
		GN845PWBB_STAGE_INIT_DONE,
	};

	constexpr uint8_t P2IO_HEADER_MAGIC = 0xaa;

	constexpr uint8_t P2IO_STATUS_OK = 0;

	constexpr uint32_t P2IO_JAMMA_IO_TEST = 0x10000000;
	constexpr uint32_t P2IO_JAMMA_IO_COIN1 = 0x20000000;
	constexpr uint32_t P2IO_JAMMA_IO_COIN2 = 0x80000000;
	constexpr uint32_t P2IO_JAMMA_IO_SERVICE = 0x40000000;
	constexpr uint32_t P2IO_JAMMA_IO_SERVICE2 = 0x00000080;

	constexpr uint32_t P2IO_JAMMA_GF_P1_START = 0x00000100;
	constexpr uint32_t P2IO_JAMMA_GF_P1_PICK = 0x00000200;
	constexpr uint32_t P2IO_JAMMA_GF_P1_WAILING = 0x00000400;
	constexpr uint32_t P2IO_JAMMA_GF_P1_EFFECT2 = 0x00000800;
	constexpr uint32_t P2IO_JAMMA_GF_P1_EFFECT1 = 0x00001000;
	constexpr uint32_t P2IO_JAMMA_GF_P1_EFFECT3 = P2IO_JAMMA_GF_P1_EFFECT1 | P2IO_JAMMA_GF_P1_EFFECT2;
	constexpr uint32_t P2IO_JAMMA_GF_P1_R = 0x00002000;
	constexpr uint32_t P2IO_JAMMA_GF_P1_G = 0x00004000;
	constexpr uint32_t P2IO_JAMMA_GF_P1_B = 0x00008000;

	constexpr uint32_t P2IO_JAMMA_GF_P2_START = 0x00010000;
	constexpr uint32_t P2IO_JAMMA_GF_P2_PICK = 0x00020000;
	constexpr uint32_t P2IO_JAMMA_GF_P2_WAILING = 0x00040000;
	constexpr uint32_t P2IO_JAMMA_GF_P2_EFFECT2 = 0x00080000;
	constexpr uint32_t P2IO_JAMMA_GF_P2_EFFECT1 = 0x00100000;
	constexpr uint32_t P2IO_JAMMA_GF_P2_EFFECT3 = P2IO_JAMMA_GF_P2_EFFECT1 | P2IO_JAMMA_GF_P2_EFFECT2;
	constexpr uint32_t P2IO_JAMMA_GF_P2_R = 0x00200000;
	constexpr uint32_t P2IO_JAMMA_GF_P2_G = 0x00400000;
	constexpr uint32_t P2IO_JAMMA_GF_P2_B = 0x00800000;

	constexpr uint32_t P2IO_JAMMA_DM_START = 0x00000100;
	constexpr uint32_t P2IO_JAMMA_DM_HIHAT = 0x00000200;
	constexpr uint32_t P2IO_JAMMA_DM_SNARE = 0x00000400;
	constexpr uint32_t P2IO_JAMMA_DM_HIGH_TOM = 0x00000800;
	constexpr uint32_t P2IO_JAMMA_DM_LOW_TOM = 0x00001000;
	constexpr uint32_t P2IO_JAMMA_DM_CYMBAL = 0x00002000;
	constexpr uint32_t P2IO_JAMMA_DM_BASS_DRUM = 0x00008000;
	constexpr uint32_t P2IO_JAMMA_DM_SELECT_L = 0x00080000;
	constexpr uint32_t P2IO_JAMMA_DM_SELECT_R = 0x00100000;

	constexpr uint32_t P2IO_JAMMA_DDR_P1_START = 0x00000100;
	constexpr uint32_t P2IO_JAMMA_DDR_P1_LEFT = 0x00004000;
	constexpr uint32_t P2IO_JAMMA_DDR_P1_RIGHT = 0x00008000;
	constexpr uint32_t P2IO_JAMMA_DDR_P1_FOOT_UP = 0x00000200;
	constexpr uint32_t P2IO_JAMMA_DDR_P1_FOOT_DOWN = 0x00000400;
	constexpr uint32_t P2IO_JAMMA_DDR_P1_FOOT_LEFT = 0x00000800;
	constexpr uint32_t P2IO_JAMMA_DDR_P1_FOOT_RIGHT = 0x00001000;

	constexpr uint32_t P2IO_JAMMA_DDR_P2_START = 0x00010000;
	constexpr uint32_t P2IO_JAMMA_DDR_P2_LEFT = 0x00400000;
	constexpr uint32_t P2IO_JAMMA_DDR_P2_RIGHT = 0x00800000;
	constexpr uint32_t P2IO_JAMMA_DDR_P2_FOOT_UP = 0x00020000;
	constexpr uint32_t P2IO_JAMMA_DDR_P2_FOOT_DOWN = 0x00040000;
	constexpr uint32_t P2IO_JAMMA_DDR_P2_FOOT_LEFT = 0x00080000;
	constexpr uint32_t P2IO_JAMMA_DDR_P2_FOOT_RIGHT = 0x00100000;

	constexpr uint32_t P2IO_JAMMA_THRILLDRIVE_START = 0x00000100;
	constexpr uint32_t P2IO_JAMMA_THRILLDRIVE_GEARSHIFT_DOWN = 0x00000200;
	constexpr uint32_t P2IO_JAMMA_THRILLDRIVE_GEARSHIFT_UP = 0x00000400;

	constexpr uint32_t P2IO_JAMMA_TOYSMARCH_P1_START = 0x00000100;
	constexpr uint32_t P2IO_JAMMA_TOYSMARCH_P1_LEFT = 0x00000800;
	constexpr uint32_t P2IO_JAMMA_TOYSMARCH_P1_RIGHT = 0x00001000;

	constexpr uint32_t P2IO_JAMMA_TOYSMARCH_P2_START = 0x00010000;
	constexpr uint32_t P2IO_JAMMA_TOYSMARCH_P2_LEFT = 0x00080000;
	constexpr uint32_t P2IO_JAMMA_TOYSMARCH_P2_RIGHT = 0x00100000;

	constexpr uint32_t P2IO_JAMMA_DANCE864_P1_START = 0x00000100;
	constexpr uint32_t P2IO_JAMMA_DANCE864_P1_LEFT = 0x00002000;
	constexpr uint32_t P2IO_JAMMA_DANCE864_P1_RIGHT = 0x00004000;
	constexpr uint32_t P2IO_JAMMA_DANCE864_P1_PAD_LEFT = 0x00000800;
	constexpr uint32_t P2IO_JAMMA_DANCE864_P1_PAD_CENTER = 0x00000400;
	constexpr uint32_t P2IO_JAMMA_DANCE864_P1_PAD_RIGHT = 0x00001000;

	constexpr uint32_t P2IO_JAMMA_DANCE864_P2_START = 0x00010000;
	constexpr uint32_t P2IO_JAMMA_DANCE864_P2_LEFT = 0x00200000;
	constexpr uint32_t P2IO_JAMMA_DANCE864_P2_RIGHT = 0x00400000;
	constexpr uint32_t P2IO_JAMMA_DANCE864_P2_PAD_LEFT = 0x00080000;
	constexpr uint32_t P2IO_JAMMA_DANCE864_P2_PAD_CENTER = 0x00040000;
	constexpr uint32_t P2IO_JAMMA_DANCE864_P2_PAD_RIGHT = 0x00100000;
	
	constexpr USBDescStrings python2io_desc_strings = {
		"",
	};

	constexpr uint8_t python2_dev_desc[] = {
		0x12, /*  u8 bLength; */
		0x01, /*  u8 bDescriptorType; Device */
		WBVAL(0x101), /*  u16 bcdUSB; v1.01 */

		0x00, /*  u8  bDeviceClass; */
		0x00, /*  u8  bDeviceSubClass; */
		0x00, /*  u8  bDeviceProtocol; [ low/full speeds only ] */
		0x08, /*  u8  bMaxPacketSize0; 8 Bytes */

		WBVAL(0x0000), /*  u16 idVendor; */
		WBVAL(0x7305), /*  u16 idProduct; */
		WBVAL(0x0020), /*  u16 bcdDevice */

		0, /*  u8  iManufacturer; */
		0, /*  u8  iProduct; */
		0, /*  u8  iSerialNumber; */
		0x01 /*  u8  bNumConfigurations; */
	};

	constexpr uint8_t python2_config_desc[] = {
		USB_CONFIGURATION_DESC_SIZE, // bLength
		USB_CONFIGURATION_DESCRIPTOR_TYPE, // bDescriptorType (Configuration)
		WBVAL(40), // wTotalLength
		0x01, // bNumInterfaces 1
		0x01, // bConfigurationValue
		0x00, // iConfiguration (String Index)
		USB_CONFIG_POWERED_MASK, // bmAttributes
		USB_CONFIG_POWER_MA(100), // bMaxPower

		/* Interface Descriptor */
		USB_INTERFACE_DESC_SIZE, // bLength
		USB_INTERFACE_DESCRIPTOR_TYPE, // bDescriptorType
		0, // bInterfaceNumber
		0, // bAlternateSetting
		3, // bNumEndpoints
		USB_CLASS_RESERVED, // bInterfaceClass
		0, // bInterfaceSubClass
		0, // bInterfaceProtocol
		0, // iInterface (String Index)

		USB_ENDPOINT_DESC_SIZE, // bLength
		USB_ENDPOINT_DESCRIPTOR_TYPE, // bDescriptorType
		USB_ENDPOINT_IN(3), // bEndpointAddress
		USB_ENDPOINT_TYPE_INTERRUPT, // bmAttributes
		WBVAL(16), // wMaxPacketSize
		3, // bInterval

		USB_ENDPOINT_DESC_SIZE, // bLength
		USB_ENDPOINT_DESCRIPTOR_TYPE, // bDescriptorType
		USB_ENDPOINT_IN(1), // bEndpointAddress
		USB_ENDPOINT_TYPE_BULK, // bmAttributes
		WBVAL(64), // wMaxPacketSize
		10, // bInterval

		USB_ENDPOINT_DESC_SIZE, // bLength
		USB_ENDPOINT_DESCRIPTOR_TYPE, // bDescriptorType
		USB_ENDPOINT_OUT(2), // bEndpointAddress
		USB_ENDPOINT_TYPE_BULK, // bmAttributes
		WBVAL(64), // wMaxPacketSize
		10, // bInterval

		//0x34 // Junk data that's in the descriptor. Uncommenting this breaks things.
	};

	struct Python2State
	{
		explicit Python2State(u32 port_);
		
		USBDevice dev{};
		USBDesc desc{};
		USBDescDevice desc_dev{};

		std::unique_ptr<input_device> devices[2];

		u32 port = 0;

		// For Thrill Drive 3
		const static int32_t wheelCenter = 0x7fdf;

		std::vector<uint8_t> buf;

		bool isMinimaidConnected = false;
		bool isUsingBtoolLights = false;

		u64 buttonState = 0;
		
		struct freeze
		{
			int ep = 0;

			int gameType = 0, prevGameType = 0;
			uint32_t jammaIoStatus = 0xf0ffff80; // Default state of real hardware
			bool force31khz = false;

			uint32_t coinsInserted[2] = {0, 0};
			bool coinButtonHeld[2] = {false, false};

			char dipSwitch[4] = {'0', '0', '0', '0'};

			int requestedDongle = -1;
			bool isDongleSlotLoaded[2] = {false, false};
			uint8_t dongleSlotPayload[2][40] = {{0}, {0}};

			std::string cardFilenames[2];

			uint32_t jammaUpdateCounter = 0;

			// For Thrill Drive 3
			int32_t wheel = wheelCenter;
			int32_t brake = 0;
			int32_t accel = 0;

			// For Guitar Freaks
			int32_t knobs[2] = {0, 0};

			// For DDR
			uint8_t oldLightCabinet = 0;

			// For Dance 86.4
			uint8_t stageMask[2] = {0xff, 0xff};
			struct
			{
				int DO = 0;
				int clk = 0;
				int shift = 0;
				int state = 0;
				int bit = 0;
			} stageState[2];
		} f;
	};

	static void usb_python2_unrealize(USBDevice* dev)
	{
		Python2State* s = USB_CONTAINER_OF(dev, Python2State, dev);

		delete s;
	}

	static uint8_t calc_crc8(uint8_t* data, const uint8_t data_size, const uint8_t crc_init)
	{
		uint8_t crc = crc_init;

		for (uint8_t index = 0; index < data_size; ++index)
		{
			uint8_t inByte = data[index];
			for (uint8_t bitPosition = 0; bitPosition < 8; ++bitPosition)
			{
				const uint8_t mix = (crc ^ inByte) & 0x01;
				crc >>= 1;
				if (mix != 0)
					crc ^= 0x8C;
				inByte >>= 1;
			}
		}
		return crc;
	}
	
	static bool read_dongle_data(FILE* infile, uint8_t* output)
	{
		bool is_valid = false;
		uint8_t temp[40] = {};
		uint8_t serial[8] = {};
		uint8_t payload[32] = {};

		std::fread(temp, 1, 40, infile);

		// Check CRC of what should be the serial and payload in two different
		// configurations and then reorder file data if required.
		// MAME format is 0x20 bytes for payload followed by 0x08 for serial.
		// The format used in other dumps for Python 2 games is the reverse of that.
		if (((~calc_crc8(temp, 0x1f, 0xff)) & 0xff) == temp[0x1f] && calc_crc8(temp + 0x20, 7, 0) == temp[0x27])
		{
			memcpy(payload, temp, 0x20);
			memcpy(serial, temp + 0x20, 8);
			DevCon.WriteLn("Dongle type MAME");
			is_valid = true;
		}
		else if (calc_crc8(temp, 7, 0) == temp[7] && ((~calc_crc8(temp + 8, 0x1f, 0xff)) & 0xff) == temp[0x27])
		{
			memcpy(serial, temp, 8);
			memcpy(payload, temp + 8, 0x20);
			DevCon.WriteLn("Dongle type OLD");
			is_valid = true;
		}

		if (is_valid)
		{
			memcpy(output, serial, 8);
			memcpy(output + 8, payload, 32);
		}
		else
		{
			memset(output, 0, 40);
			DevCon.Error("Dongle BAD: invalid CRC values");
		}

		return is_valid;
	}

	static void initialize_device(USBDevice* dev)
	{
		Python2State* s = USB_CONTAINER_OF(dev, Python2State, dev);

		// It seems like the device is recreated from scratch every time the config dialog is closed so this isn't all that helpful after all
		if (s->f.gameType != s->f.prevGameType && s->devices[0] != nullptr)
			s->devices[0].reset();

		if (s->f.gameType != s->f.prevGameType && s->devices[1] != nullptr)
			s->devices[1].reset();

		s->f.prevGameType = s->f.gameType;

		if (s->f.gameType == GAMETYPE_DDR)
		{
			if (s->devices[0] == nullptr)
			{
				s->devices[0] = std::make_unique<extio_device>();
			}

			if (s->devices[1] == nullptr)
			{
				auto aciodev = std::make_unique<acio_device>();
				aciodev->add_acio_device(1, std::make_unique<acio_icca_device>(dev, s->f.cardFilenames[0]));
				aciodev->add_acio_device(2, std::make_unique<acio_icca_device>(dev, s->f.cardFilenames[1]));
				s->devices[1] = std::move(aciodev);
			}
		}
	}

	static void load_configuration(USBDevice* dev)
	{
		Python2State* s = USB_CONTAINER_OF(dev, Python2State, dev);

		SettingsInterface* si = Host::GetSettingsInterface();

		s->f.dipSwitch[0] = si->GetBoolValue("Python2/Game", "DIPSW1", false) ? '1' : '0';
		s->f.dipSwitch[1] = si->GetBoolValue("Python2/Game", "DIPSW2", false) ? '1' : '0';
		s->f.dipSwitch[2] = si->GetBoolValue("Python2/Game", "DIPSW3", false) ? '1' : '0';
		s->f.dipSwitch[3] = si->GetBoolValue("Python2/Game", "DIPSW4", false) ? '1' : '0';
		s->f.force31khz = si->GetBoolValue("Python2/Game", "Force31kHz", false);

		DevCon.WriteLn("dipswitches: %c %c %c %c\n", s->f.dipSwitch[0], s->f.dipSwitch[1], s->f.dipSwitch[2], s->f.dipSwitch[3]);
		DevCon.WriteLn("force31khz: %d\n", s->f.force31khz);

		std::string iLinkIdPath = si->GetStringValue("Python2/System", "ILinkIdFile", "");
		DevCon.WriteLn("IlinkIdPath: %s", iLinkIdPath.c_str());

		s->f.cardFilenames[0] = si->GetStringValue("Python2/Game", "Player1CardFile", "card1.txt");
		DevCon.WriteLn("Player 1 card filename: %s", s->f.cardFilenames[0].c_str());

		s->f.cardFilenames[1] = si->GetStringValue("Python2/Game", "Player2CardFile", "card2.txt");
		DevCon.WriteLn("Player 2 card filename: %s", s->f.cardFilenames[0].c_str());

		s->f.gameType = si->GetIntValue("Python2/Game", "GameType", 0);
		DevCon.WriteLn("GameType: %d", s->f.gameType);

		const std::string hddIdPath = si->GetStringValue("DEV9/Hdd", "HddIdFile", "");
		DevCon.WriteLn("HddIdPath: %s", hddIdPath.c_str());
		if (!hddIdPath.empty())
		{
			if (FileSystem::FileExists(hddIdPath.c_str()))
				EmuConfig.DEV9.HddIdFile = hddIdPath;
		}

		auto dongleBlackPath = si->GetStringValue("Python2/Game", "DongleBlackFile", "");
		DevCon.WriteLn("DongleBlackPath: %s", dongleBlackPath.c_str());
		if (!dongleBlackPath.empty())
		{
			auto dongleFile = FileSystem::OpenManagedCFile(dongleBlackPath.c_str(), "rb");
			if (dongleFile && FileSystem::FSize64(dongleFile.get()) >= 40)
			{
				s->f.isDongleSlotLoaded[0] = true;
				s->f.isDongleSlotLoaded[0] = read_dongle_data(dongleFile.get(), &s->f.dongleSlotPayload[0][0]);
			}
			else
			{
				std::fill(std::begin(s->f.dongleSlotPayload[0]), std::end(s->f.dongleSlotPayload[0]), 0);
				s->f.isDongleSlotLoaded[0] = false;
			}
		}

		auto dongleWhitePath = si->GetStringValue("Python2/Game", "DongleWhiteFile", "");
		DevCon.WriteLn("DongleWhitePath: %s", dongleWhitePath.c_str());
		if (!dongleWhitePath.empty())
		{
			auto dongleFile = FileSystem::OpenManagedCFile(dongleWhitePath.c_str(), "rb");
			if (dongleFile && FileSystem::FSize64(dongleFile.get()) >= 40)
			{
				s->f.isDongleSlotLoaded[1] = read_dongle_data(dongleFile.get(), &s->f.dongleSlotPayload[1][0]);
			}
			else
			{
				std::fill(std::begin(s->f.dongleSlotPayload[1]), std::end(s->f.dongleSlotPayload[1]), 0);
				s->f.isDongleSlotLoaded[1] = false;
			}
		}
	}

	static void usb_python2_handle_reset(USBDevice* dev)
	{
		Python2State* s = USB_CONTAINER_OF(dev, Python2State, dev);

		// Initialize all variables to try and keep a consistent state
		s->buf.clear();
		s->isMinimaidConnected = false;
		s->isUsingBtoolLights = false;

		s->f.gameType = s->f.prevGameType = -1;
		s->f.jammaIoStatus = 0xf0ffff80;
		s->f.force31khz = false;
		s->f.coinsInserted[0] = s->f.coinsInserted[1] = 0;
		s->f.coinButtonHeld[0] = s->f.coinButtonHeld[1] = false;
		memset(s->f.dipSwitch, '0', sizeof(s->f.dipSwitch));
		s->f.requestedDongle = -1;
		s->f.isDongleSlotLoaded[0] = s->f.isDongleSlotLoaded[1] = false;
		memset(s->f.dongleSlotPayload[0], 0, sizeof(s->f.dongleSlotPayload[0]));
		memset(s->f.dongleSlotPayload[1], 0, sizeof(s->f.dongleSlotPayload[1]));
		s->f.cardFilenames[0] = s->f.cardFilenames[1] = "";
		s->f.jammaUpdateCounter = 0;
		s->f.wheel = s->wheelCenter;
		s->f.brake = s->f.accel = 0;
		s->f.knobs[0] = s->f.knobs[1] = 0;
		s->f.oldLightCabinet = 0;

		for (int i = 0; i < 2; i++)
		{
			s->f.stageMask[i] = 0xff;
			s->f.stageState[i].DO = 0;
			s->f.stageState[i].clk = 0;
			s->f.stageState[i].shift = 0;
			s->f.stageState[i].state = GN845PWBB_STAGE_IDLE;
			s->f.stageState[i].bit = 0;
		}

		// Load the configuration and start SPDIF patcher thread every time a game is started
		load_configuration(dev);
		initialize_device(dev);
	}

	static void usb_python2_handle_control(USBDevice* dev, USBPacket* p,
	int request, int value, int index, int length, uint8_t* data)
	{
		const int ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
		if (ret >= 0)
			return;

		DevCon.WriteLn("usb-python2: Unimplemented handle control request! %04x\n", request);
		p->status = USB_RET_STALL;
	}

	static void gn845pwbb_do_w(Python2State* s, int offset, int data)
	{
		s->f.stageState[offset].DO = !data;
	}

	static void gn845pwbb_clk_w(Python2State* s, int offset, int data)
	{
		// Based on implementation from MAME's ksys573.cpp
		int clk = !data;
		if (clk != s->f.stageState[offset].clk)
		{
			s->f.stageState[offset].clk = clk;

			if (clk)
			{
				s->f.stageState[offset].shift = (s->f.stageState[offset].shift >> 1) | (s->f.stageState[offset].DO << 12);

				switch (s->f.stageState[offset].state)
				{
					case GN845PWBB_STAGE_IDLE:
						if (s->f.stageState[offset].shift == 0xc90)
						{
							s->f.stageState[offset].state = GN845PWBB_STAGE_INIT;
							s->f.stageState[offset].bit = 0;
							s->f.stageMask[offset] = 0xf9;
						}
					break;

					case GN845PWBB_STAGE_INIT:
						s->f.stageState[offset].bit++;
					if (s->f.stageState[offset].bit < 22)
					{
						s->f.stageMask[offset] = ~0x12;

						if (s->f.stageState[offset].bit - 1 < 2)
							s->f.stageMask[offset] |= 0x10;

						s->f.stageMask[offset] |= 1 << (s->f.stageState[offset].bit & 1);
					}
					else
					{
						s->f.stageState[offset].bit = 0;
						s->f.stageState[offset].state = GN845PWBB_STAGE_INIT_DONE;
						s->f.stageMask[offset] = 0xff;
					}
					break;
				}
			}
		}

		if (s->f.stageState[offset].state != GN845PWBB_STAGE_INIT_DONE)
		{
			s->f.jammaIoStatus = s->f.jammaIoStatus & 0xff0000ff;
			s->f.jammaIoStatus |= s->f.stageMask[0] << 8;
			s->f.jammaIoStatus |= s->f.stageMask[1] << 16;
		}

		// printf("stage: %dp data clk=%d state=%d d0=%d shift=%08x bit=%d stage_mask=%02x %02x\n", offset + 1, clk,
		// 	s->f.stageState[offset].state, s->f.stageState[offset].DO, s->f.stageState[offset].shift, s->f.stageState[offset].bit, s->f.stageMask[0], s->f.stageMask[1]);
	}

	static void p2io_cmd_handler(USBDevice* dev, USBPacket* p, std::vector<uint8_t>& data)
	{
		Python2State* s = USB_CONTAINER_OF(dev, Python2State, dev);

		// Remove any garbage from beginning of buffer if it exists
		for (size_t i = 0; i < s->buf.size(); i++)
		{
			if (s->buf[i] == P2IO_HEADER_MAGIC)
			{
				if (i != 0)
					s->buf.erase(s->buf.begin(), s->buf.begin() + i);

				break;
			}
		}

		if (s->buf.size() < sizeof(P2IO_PACKET_HEADER))
			return;

		const P2IO_PACKET_HEADER* header = reinterpret_cast<P2IO_PACKET_HEADER*>(s->buf.data());
		const size_t totalPacketLen = header->len + 2; // header byte + sequence byte

		if (s->buf.size() >= totalPacketLen && header->magic == P2IO_HEADER_MAGIC)
		{
			data.push_back(header->seqNo);
			data.push_back(P2IO_STATUS_OK); // Status

			if (header->cmd == P2IO_CMD_GET_VERSION)
			{
				DevCon.WriteLn("p2io: P2IO_CMD_GET_VERSION");

				// Get version returns D44:1.6.4
				const uint8_t resp[] = {
					'D', '4', '4', '\0', // product code
					1, // major
					6, // minor
					4 // revision
				};
				data.insert(data.end(), std::begin(resp), std::end(resp));
			}
			else if (header->cmd == P2IO_CMD_SET_AV_MASK)
			{
				DevCon.WriteLn("p2io: P2IO_CMD_SET_AV_MASK %02x", s->buf[4]);
				data.push_back(0);
			}
			else if (header->cmd == P2IO_CMD_GET_AV_REPORT)
			{
				DevCon.WriteLn("p2io: P2IO_CMD_GET_AV_REPORT");
				data.push_back(s->f.force31khz ? P2IO_AVREPORT_MODE_31KHZ : P2IO_AVREPORT_MODE_15KHZ);
			}
			else if (header->cmd == P2IO_CMD_DALLAS)
			{
				const auto subCmd = s->buf[4];
				DevCon.WriteLn("p2io: P2IO_CMD_DALLAS_CMD %02x", subCmd);

				if (subCmd == 0 || subCmd == 1 || subCmd == 2)
				{
					// Dallas Read SID/Mem
					if (subCmd != 2)
						s->f.requestedDongle = subCmd;

					if (s->f.requestedDongle >= 0)
					{
						data.push_back(s->f.isDongleSlotLoaded[s->f.requestedDongle]);
						data.insert(data.end(), std::begin(s->f.dongleSlotPayload[s->f.requestedDongle]), std::end(s->f.dongleSlotPayload[s->f.requestedDongle]));
					}
					else
					{
						data.push_back(0);
						data.insert(data.end(), s->buf.begin() + 5, s->buf.begin() + 5 + 40); // Return received data in buffer
					}
				}
				else if (subCmd == 3)
				{
					// TODO: is this ever used?
					// Dallas Write Mem
					data.push_back(0);
					data.insert(data.end(), s->buf.begin() + 5, s->buf.begin() + 5 + 40); // Return received data in buffer
				}
			}
			else if (header->cmd == P2IO_CMD_READ_DIPSWITCH)
			{
				DevCon.WriteLn("P2IO_CMD_READ_DIPSWITCH %02x", s->buf[4]);

				uint8_t val = 0;
				for (size_t i = 0; i < 4; i++)
					val |= (1 << (3 - i)) * (s->f.dipSwitch[i] == '1');

				data.push_back(val & 0x7f); // 0xff is ignored
			}

			else if (header->cmd == P2IO_CMD_COIN_STOCK)
			{
				// Python2ConVerbose.WriteLn("P2IO_CMD_COIN_STOCK");

				const uint8_t resp[] = {
					0, // If this is non-zero then the following 4 bytes are not processed
					uint8_t((s->f.coinsInserted[0] >> 8)),
					uint8_t(s->f.coinsInserted[0]),
					uint8_t((s->f.coinsInserted[1] >> 8)),
					uint8_t(s->f.coinsInserted[1]),
				};
				data.insert(data.end(), std::begin(resp), std::end(resp));
			}
			else if (header->cmd == P2IO_CMD_COIN_COUNTER && (s->buf[4] & 0x10) == 0x10)
			{
				// p2sub_coin_counter_merge exists which calls "32 10" and "32 11"
				DevCon.WriteLn("p2io: P2IO_CMD_COIN_COUNTER_MERGE %02x %02x", s->buf[4], s->buf[5]);
				data.push_back(0);
			}
			else if (header->cmd == P2IO_CMD_COIN_COUNTER)
			{
				// p2sub_coin_counter accepts parameters with a range of 0x00 to 0x0f
				DevCon.WriteLn("p2io: P2IO_CMD_COIN_COUNTER %02x %02x", s->buf[4], s->buf[5]);
				data.push_back(0);
			}
			else if (header->cmd == P2IO_CMD_COIN_BLOCKER)
			{
				// Param 1 seems to either be 0/1
				DevCon.WriteLn("p2io: P2IO_CMD_COIN_BLOCKER %02x %02x", s->buf[4], s->buf[5]);
				data.push_back(0);
			}
			else if (header->cmd == P2IO_CMD_COIN_COUNTER_OUT)
			{
				// Param 1 seems to either be 0/1
				DevCon.WriteLn("p2io: P2IO_CMD_COIN_COUNTER_OUT %02x %02x", s->buf[4], s->buf[5]);
				data.push_back(0);
			}

			else if (header->cmd == P2IO_CMD_LAMP_OUT && s->buf[4] == 0xff)
			{
				DevCon.WriteLn("p2io: P2IO_CMD_LAMP_OUT_ALL %08x", *(int*)&s->buf[5]);

				data.push_back(0);
			}
			else if (header->cmd == P2IO_CMD_LAMP_OUT)
			{
				// printf("LAMP_OUT: %02x %02x [%d %d] [%d %d] %08x %d\n", s->buf[4], s->buf[5], s->buf[5] & 8, s->buf[5] & 4, s->buf[5] & 2, s->buf[5] & 1, s->f.jammaIoStatus, s->f.gameType == GAMETYPE_DANCE864);

				if (s->f.gameType == GAMETYPE_DANCE864 && s->buf[4] == 1)
				{
					gn845pwbb_do_w(s, 0, !!!(s->buf[5] & 1));
					gn845pwbb_clk_w(s, 0, !!(s->buf[5] & 2));

					gn845pwbb_do_w(s, 1, !!!(s->buf[5] & 4));
					gn845pwbb_clk_w(s, 1, !!(s->buf[5] & 8));
				}
				else if (s->f.gameType == GAMETYPE_DDR)
				{
					// 73 is 0111 0011 // p1 halogen up
					// b3 is 1011 0011 // p1 halogen down
					// d3 is 1101 0011 // p2 halogen up
					// e3 is 1110 0011 // p2 halogen down
					// f1 is 1111 0001 // p1
					// f2 is 1111 0010 // p2
					// f3 is 1111 0011 // p1 + p2
					// 53 is 0101 0011 // p1 halogen up + p2 halogen up
					// b0 is 1011 0000 // p1 halogen down + p1 + p2 start
					// f0 is 1111 0000 // p1 + p2 seen from p2io. Mask ???
					// f3 is 1111 0011 // dunno what this is. Maybe bass lights???
					// 03 is 0000 0011 // halogen lights seen from P2io. Mask???
					// 00 is 0000 0000 // all lights
					//            XX   // don't care
				}

				data.push_back(0);
			}

			else if (header->cmd == P2IO_CMD_PORT_READ)
			{
				// TODO: What port?
				DevCon.WriteLn("p2io: P2IO_CMD_PORT_READ %02x", s->buf[4]);
				data.push_back(0);
			}
			else if (header->cmd == P2IO_CMD_PORT_READ_POR)
			{
				// TODO: What port?
				DevCon.WriteLn("p2io: P2IO_CMD_PORT_READ_POR %02x", s->buf[4]);
				data.push_back(0);
			}
			else if (header->cmd == P2IO_CMD_SEND_IR)
			{
				DevCon.WriteLn("p2io: P2IO_CMD_SEND_IR %02x", s->buf[4]);
				data.push_back(0);
			}
			else if (header->cmd == P2IO_CMD_SET_WATCHDOG)
			{
				DevCon.WriteLn("p2io: P2IO_CMD_SET_WATCHDOG %02x", s->buf[4]);
				data.push_back(0);
			}
			else if (header->cmd == P2IO_CMD_JAMMA_START)
			{
				DevCon.WriteLn("p2io: P2IO_CMD_JAMMA_START");
				data.push_back(0); // 0 = succeeded, anything else = fail
			}
			else if (header->cmd == P2IO_CMD_GET_JAMMA_POR)
			{
				DevCon.WriteLn("p2io: P2IO_CMD_GET_JAMMA_POR");
				const uint8_t resp[] = {0, 0, 0, 0}; // Real hardware returns 0x00ff 0x00ff?
				data.insert(data.end(), std::begin(resp), std::end(resp));
			}
			else if (header->cmd == P2IO_CMD_FWRITEMODE && s->buf[4] == 0xaa)
			{
				// WARNING: FIRMWARE WRITE MODE! Be careful with testing this on real hardware!
				// The device will restart/disconnect(?) if this is called and lights seem to go out.
				// In the P2IO driver, usbboot_init is called after this command so it must be reconnected to be usable again.
				// If p2sub_setmode is called with a value of 0x20 then the P2IO will go into firmware write mode. A value of 2 calls inits the device with P2IO_CMD_JAMMA_START.
				DevCon.WriteLn("p2io: P2IO_CMD_FWRITEMODE");
				data.push_back(0); // 0 = succeeded, anything else = fail
			}

			else if (header->cmd == P2IO_CMD_SCI_SETUP)
			{
				const auto port = s->buf[4];
				const auto cmd = s->buf[5];
				const auto param = s->buf[6];

				DevCon.WriteLn("p2io: P2IO_CMD_SCI_OPEN %02x %02x %02x", port, cmd, param);
				data.push_back(0);

				const auto device = s->devices[port].get();
				if (device != nullptr)
				{
					if (cmd == 0)
						device->open();
					else if (header->cmd == 0xff)
						device->close();
				}
			}
			else if (header->cmd == P2IO_CMD_SCI_WRITE)
			{
				const auto port = s->buf[4];
				const auto packetLen = s->buf[5];

#ifdef PCSX2_DEVBUILD
				DevCon.WriteLn("p2io: P2IO_CMD_SCI_WRITE: ");
				for (size_t i = 0; i < s->buf.size(); i++)
				{
					printf("%02x ", s->buf[i]);
				}
				printf("\n");
#endif

				const auto device = s->devices[port].get();
				if (device != nullptr)
				{
					const auto startIdx = s->buf.begin() + 6;
					auto escapedPacket = acio_unescape_packet(std::vector<uint8_t>(startIdx, startIdx + packetLen));
					device->write(escapedPacket);
				}

				data.push_back(packetLen);
			}
			else if (header->cmd == P2IO_CMD_SCI_READ)
			{
				const auto port = s->buf[4];
				const auto requestedLen = s->buf[5];

				//Python2Con.WriteLn("P2IO_CMD_SCI_READ %02x %02x\n", port, requestedLen);

				const auto packetLenOffset = data.size();
				data.push_back(0);

				const auto device = s->devices[port].get();
				auto readLen = 0;
				if (device != nullptr && requestedLen > 0)
				{
					readLen = device->read(data, requestedLen);
				}

				data[packetLenOffset] = readLen;
			}
			else
			{
#ifdef PCSX2_DEVBUILD
				printf("usb_python2_handle_data %zx\n", s->buf.size());
				for (size_t i = 0; i < s->buf.size(); i++)
				{
					printf("%02x ", s->buf[i]);
				}
				printf("\n");
#endif
			}

			data.insert(data.begin(), data.size());
			data = acio_escape_packet(data);
			data.insert(data.begin(), P2IO_HEADER_MAGIC);

			s->buf.erase(s->buf.begin(), s->buf.begin() + totalPacketLen);
		}
	}

	static void usb_python2_handle_data(USBDevice* dev, USBPacket* p)
	{
		Python2State* s = USB_CONTAINER_OF(dev, Python2State, dev);

		switch (p->pid)
		{
			case USB_TOKEN_IN:
			{
				std::vector<uint8_t> data;

				if (p->ep->nr == 3) // JAMMA IO pipe
				{
					// Real hardware seems to have the analog values non-0 when no I/O is attached.
					// Here is the results of 20 consecutive reads with no I/O attached.
					// The values are roughly the same but there's some small movement between reads. Possibly just noise.
					// 10 40 10 f0 10 80 10 c0
					// 10 40 11 00 10 80 10 d0
					// 10 50 11 00 10 80 10 c0
					// 10 40 11 00 10 80 10 d0
					// 10 40 11 00 10 70 10 c0
					// 10 40 11 00 10 80 10 c0
					// 10 40 11 00 10 80 10 e0
					// 10 40 11 00 10 80 10 d0
					// 10 40 11 00 10 80 10 d0
					// 10 40 11 00 10 80 10 e0
					// 10 40 11 00 10 80 10 c0
					// 10 40 11 00 10 90 10 c0
					// 10 30 11 00 10 80 10 c0
					// 10 20 11 00 10 90 10 d0
					// 10 10 11 00 10 90 10 c0
					// 10 10 11 00 10 90 10 d0
					// 10 10 11 00 10 90 10 c0
					// 10 00 11 00 10 80 10 e0
					// 10 00 11 00 10 90 10 d0
					// 10 20 11 00 10 90 10 c0
					uint8_t resp[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
					uint32_t* jammaIo = reinterpret_cast<uint32_t*>(&resp[0]);
					uint16_t* analogIo = reinterpret_cast<uint16_t*>(&resp[4]);

					s->f.jammaIoStatus ^= 2; // Watchdog, if this bit isn't flipped every update then a security error will be raised

#define CheckKeyState(key, val) \
	{ \
		if (Python2Device::GetInputState(dev, key)) \
			s->f.jammaIoStatus &= ~(val); \
		else \
			s->f.jammaIoStatus |= (val); \
	}

#define CheckKeyStateOneShot(key, val) \
	{ \
		if (Python2Device::GetInputState(dev, key)) \
			s->f.jammaIoStatus &= ~(val); \
		else \
			s->f.jammaIoStatus |= (val); \
	}

#define KnobStateInc(key, val, playerId) \
	{ \
		if (Python2Device::GetInputState(dev, key)) \
			s->f.knobs[(playerId)] = (s->f.knobs[(playerId)] + 1) % 4; \
	}

#define KnobStateDec(key, val, playerId) \
	{ \
		if (Python2Device::GetInputState(dev, key)) \
			s->f.knobs[(playerId)] = (s->f.knobs[(playerId)] - 1) < 0 ? 3 : (s->f.knobs[(playerId)] - 1); \
	}

#define CoinInc(key, idx) \
	{ \
		if (!(s->f.jammaIoStatus & (key))) \
		{ \
			if (!s->f.coinButtonHeld[(idx)]) \
			{ \
				s->f.coinsInserted[(idx)]++; \
				s->f.coinButtonHeld[(idx)] = true; \
			} \
		} \
		else \
		{ \
			s->f.coinButtonHeld[(idx)] = false; \
		} \
	}

					// Handle inputs that shouldn't be oneshots typically on every update
					CheckKeyState(BID_TEST, P2IO_JAMMA_IO_TEST);
					CheckKeyState(BID_SERVICE, P2IO_JAMMA_IO_SERVICE);
					CheckKeyState(BID_COIN1, P2IO_JAMMA_IO_COIN1);
					CheckKeyState(BID_COIN2, P2IO_JAMMA_IO_COIN2);

					// Python 2 games only accept coins via the P2IO directly, even though the game sees the JAMMA coin buttons returned here(?)
					CoinInc(P2IO_JAMMA_IO_COIN1, 0);
					CoinInc(P2IO_JAMMA_IO_COIN2, 1);

					if (s->f.gameType == GAMETYPE_DDR)
					{
						CheckKeyState(BID_DDR_P1_START, P2IO_JAMMA_DDR_P1_START);
						CheckKeyState(BID_DDR_P1_SELECT_LEFT, P2IO_JAMMA_DDR_P1_LEFT);
						CheckKeyState(BID_DDR_P1_SELECT_RIGHT, P2IO_JAMMA_DDR_P1_RIGHT);
						CheckKeyState(BID_DDR_P1_FOOT_LEFT, P2IO_JAMMA_DDR_P1_FOOT_LEFT);
						CheckKeyState(BID_DDR_P1_FOOT_RIGHT, P2IO_JAMMA_DDR_P1_FOOT_DOWN);
						CheckKeyState(BID_DDR_P1_FOOT_UP, P2IO_JAMMA_DDR_P1_FOOT_UP);
						CheckKeyState(BID_DDR_P1_FOOT_DOWN, P2IO_JAMMA_DDR_P1_FOOT_RIGHT);

						CheckKeyState(BID_DDR_P2_START, P2IO_JAMMA_DDR_P2_START);
						CheckKeyState(BID_DDR_P2_SELECT_LEFT, P2IO_JAMMA_DDR_P2_LEFT);
						CheckKeyState(BID_DDR_P2_SELECT_RIGHT, P2IO_JAMMA_DDR_P2_RIGHT);
						CheckKeyState(BID_DDR_P2_FOOT_LEFT, P2IO_JAMMA_DDR_P2_FOOT_LEFT);
						CheckKeyState(BID_DDR_P2_FOOT_RIGHT, P2IO_JAMMA_DDR_P2_FOOT_DOWN);
						CheckKeyState(BID_DDR_P2_FOOT_UP, P2IO_JAMMA_DDR_P2_FOOT_UP);
						CheckKeyState(BID_DDR_P2_FOOT_DOWN, P2IO_JAMMA_DDR_P2_FOOT_RIGHT);
					}

					// Hold the state for a certain amount of updates so the game can register quick changes.
					// Setting this value too low will result in very fast key changes being dropped.
					// Setting this value too high will result in latency with key presses.
					// Only really useful for inputs that should be oneshots so that the game has enough time to process the quick change in inputs.

					s->f.jammaUpdateCounter = (s->f.jammaUpdateCounter + 1) % 10;

					jammaIo[0] = s->f.jammaIoStatus;

					data.insert(data.end(), std::begin(resp), std::end(resp));
				}
				else if (p->ep->nr == 1) // P2IO output pipe
				{
					p2io_cmd_handler(dev, p, data);
				}

				if (data.size() == 0)
					data.push_back(0);

				if (data.size() > 0)
					usb_packet_copy(p, data.data(), data.size());
				else
					p->status = USB_RET_NAK;

				break;
			}

			case USB_TOKEN_OUT:
			{
				if (p->ep->nr == 2) // P2IO input pipe
				{
					const auto len = usb_packet_size(p);
					auto buf = std::vector<uint8_t>(len);
					usb_packet_copy(p, buf.data(), buf.size());

					buf = acio_unescape_packet(buf);
					s->buf.insert(s->buf.end(), buf.begin(), buf.end());
				}
				break;
			}

			default:
				p->status = USB_RET_STALL;
				break;
		}
	}
	
	Python2State::Python2State(u32 port_) : port(port_)
	{
	}

	const char* Python2Device::Name() const
	{
		return "Python 2";
	}

	const char* Python2Device::TypeName() const
	{
		return "python2io";
	}

	bool Python2Device::Freeze(USBDevice* dev, StateWrapper& sw) const
	{
		Python2State* s = USB_CONTAINER_OF(dev, Python2State, dev);

		if (!sw.DoMarker("Python2Device"))
			return false;

		sw.DoBytes(&s->f, sizeof(Python2State::freeze));

		return !sw.HasError();
	}

	void Python2Device::UpdateSettings(USBDevice* dev, SettingsInterface& si) const
	{
		load_configuration(dev);
	}


	float Python2Device::GetBindingValue(const USBDevice* dev, u32 bind) const
	{
		Python2State* s = USB_CONTAINER_OF(dev, Python2State, dev);

		const u64 bit = 1ull << static_cast<u64>(bind);
		return ((s->buttonState & bit) != 0) ? 1.0f : 0.0f;
	}

	void Python2Device::SetBindingValue(USBDevice* dev, u32 bind, float value) const
	{
		Python2State* s = USB_CONTAINER_OF(dev, Python2State, dev);

		const u64 bit = 1ull << static_cast<u64>(bind);
		if (value >= 0.5f)
			s->buttonState |= bit;
		else
			s->buttonState &= ~bit;
	}

	std::span<const InputBindingInfo> Python2Device::Bindings(u32 subtype) const
	{
		static constexpr InputBindingInfo bindings[] = {
			{"Test", "Test Button", nullptr, InputBindingInfo::Type::Button, BID_TEST, GenericInputBinding::Select},
			{"Service", "Service Button", nullptr, InputBindingInfo::Type::Button, BID_SERVICE, GenericInputBinding::System},
			{"Coin1", "Coin Player 1", nullptr, InputBindingInfo::Type::Button, BID_COIN1, GenericInputBinding::L3},
			{"Coin2", "Coin Player 2", nullptr, InputBindingInfo::Type::Button, BID_COIN2, GenericInputBinding::Unknown},
			
			{"DDRP1Start", "DDR Player 1 Start", nullptr, InputBindingInfo::Type::Button, BID_DDR_P1_START, GenericInputBinding::Start},
			{"DDRP1SelectLeft", "DDR Player 1 Select Left", nullptr, InputBindingInfo::Type::Button, BID_DDR_P1_SELECT_LEFT, GenericInputBinding::LeftStickLeft},
			{"DDRP1SelectRight", "DDR Player 1 Select Right", nullptr, InputBindingInfo::Type::Button, BID_DDR_P1_SELECT_RIGHT, GenericInputBinding::LeftStickRight},
			{"DDRP1FootLeft", "DDR Player 1 Foot Left", nullptr, InputBindingInfo::Type::Button, BID_DDR_P1_FOOT_LEFT, GenericInputBinding::DPadLeft},
			{"DDRP1FootRight", "DDR Player 1 Foot Right", nullptr, InputBindingInfo::Type::Button, BID_DDR_P1_FOOT_RIGHT, GenericInputBinding::Circle},
			{"DDRP1FootUp", "DDR Player 1 Foot Up", nullptr, InputBindingInfo::Type::Button, BID_DDR_P1_FOOT_UP, GenericInputBinding::DPadUp},
			{"DDRP1FootDown", "DDR Player 1 Foot Down", nullptr, InputBindingInfo::Type::Button, BID_DDR_P1_FOOT_DOWN, GenericInputBinding::Cross},

			{"DDRP2Start", "DDR Player 2 Start", nullptr, InputBindingInfo::Type::Button, BID_DDR_P2_START, GenericInputBinding::Unknown},
			{"DDRP2SelectLeft", "DDR Player 2 Select Left", nullptr, InputBindingInfo::Type::Button, BID_DDR_P2_SELECT_LEFT, GenericInputBinding::Unknown},
			{"DDRP2SelectRight", "DDR Player 2 Select Right", nullptr, InputBindingInfo::Type::Button, BID_DDR_P2_SELECT_RIGHT, GenericInputBinding::Unknown},
			{"DDRP2FootLeft", "DDR Player 2 Foot Left", nullptr, InputBindingInfo::Type::Button, BID_DDR_P2_FOOT_LEFT, GenericInputBinding::Unknown},
			{"DDRP2FootRight", "DDR Player 2 Foot Right", nullptr, InputBindingInfo::Type::Button, BID_DDR_P2_FOOT_RIGHT, GenericInputBinding::Unknown},
			{"DDRP2FootUp", "DDR Player 2 Foot Up", nullptr, InputBindingInfo::Type::Button, BID_DDR_P2_FOOT_UP, GenericInputBinding::Unknown},
			{"DDRP2FootDown", "DDR Player 2 Foot Down", nullptr, InputBindingInfo::Type::Button, BID_DDR_P2_FOOT_DOWN, GenericInputBinding::Unknown},
			
			{"KeypadP10", "Keypad Player 1 '0'", nullptr, InputBindingInfo::Type::Button, BID_KEYPADP1_0, GenericInputBinding::Unknown},
			{"KeypadP11", "Keypad Player 1 '1'", nullptr, InputBindingInfo::Type::Button, BID_KEYPADP1_1, GenericInputBinding::Unknown},
			{"KeypadP12", "Keypad Player 1 '2'", nullptr, InputBindingInfo::Type::Button, BID_KEYPADP1_2, GenericInputBinding::Unknown},
			{"KeypadP13", "Keypad Player 1 '3'", nullptr, InputBindingInfo::Type::Button, BID_KEYPADP1_3, GenericInputBinding::Unknown},
			{"KeypadP14", "Keypad Player 1 '4'", nullptr, InputBindingInfo::Type::Button, BID_KEYPADP1_4, GenericInputBinding::Unknown},
			{"KeypadP15", "Keypad Player 1 '5'", nullptr, InputBindingInfo::Type::Button, BID_KEYPADP1_5, GenericInputBinding::Unknown},
			{"KeypadP16", "Keypad Player 1 '6'", nullptr, InputBindingInfo::Type::Button, BID_KEYPADP1_6, GenericInputBinding::Unknown},
			{"KeypadP17", "Keypad Player 1 '7'", nullptr, InputBindingInfo::Type::Button, BID_KEYPADP1_7, GenericInputBinding::Unknown},
			{"KeypadP18", "Keypad Player 1 '8'", nullptr, InputBindingInfo::Type::Button, BID_KEYPADP1_8, GenericInputBinding::Unknown},
			{"KeypadP19", "Keypad Player 1 '9'", nullptr, InputBindingInfo::Type::Button, BID_KEYPADP1_9, GenericInputBinding::Unknown},
			{"KeypadP100", "Keypad Player 1 '00'", nullptr, InputBindingInfo::Type::Button, BID_KEYPADP1_00, GenericInputBinding::Unknown},
			{"KeypadP1CardIn", "Keypad Player 1 Card In", nullptr, InputBindingInfo::Type::Button, BID_KEYPADP1_CARD_IN, GenericInputBinding::R3},

			{"KeypadP20", "Keypad Player 2 '0'", nullptr, InputBindingInfo::Type::Button, BID_KEYPADP2_0, GenericInputBinding::Unknown},
			{"KeypadP21", "Keypad Player 2 '1'", nullptr, InputBindingInfo::Type::Button, BID_KEYPADP2_1, GenericInputBinding::Unknown},
			{"KeypadP22", "Keypad Player 2 '2'", nullptr, InputBindingInfo::Type::Button, BID_KEYPADP2_2, GenericInputBinding::Unknown},
			{"KeypadP23", "Keypad Player 2 '3'", nullptr, InputBindingInfo::Type::Button, BID_KEYPADP2_3, GenericInputBinding::Unknown},
			{"KeypadP24", "Keypad Player 2 '4'", nullptr, InputBindingInfo::Type::Button, BID_KEYPADP2_4, GenericInputBinding::Unknown},
			{"KeypadP25", "Keypad Player 2 '5'", nullptr, InputBindingInfo::Type::Button, BID_KEYPADP2_5, GenericInputBinding::Unknown},
			{"KeypadP26", "Keypad Player 2 '6'", nullptr, InputBindingInfo::Type::Button, BID_KEYPADP2_6, GenericInputBinding::Unknown},
			{"KeypadP27", "Keypad Player 2 '7'", nullptr, InputBindingInfo::Type::Button, BID_KEYPADP2_7, GenericInputBinding::Unknown},
			{"KeypadP28", "Keypad Player 2 '8'", nullptr, InputBindingInfo::Type::Button, BID_KEYPADP2_8, GenericInputBinding::Unknown},
			{"KeypadP29", "Keypad Player 2 '9'", nullptr, InputBindingInfo::Type::Button, BID_KEYPADP2_9, GenericInputBinding::Unknown},
			{"KeypadP200", "Keypad Player 2 '00'", nullptr, InputBindingInfo::Type::Button, BID_KEYPADP2_00, GenericInputBinding::Unknown},
			{"KeypadP2CardIn", "Keypad Player 2 Card In", nullptr, InputBindingInfo::Type::Button, BID_KEYPADP2_CARD_IN, GenericInputBinding::Unknown},	
		};

		return bindings;
	}

	std::span<const SettingInfo> Python2Device::Settings(u32 subtype) const
	{
		static constexpr SettingInfo info[] = {
			{SettingInfo::Type::Boolean, "custom_config", TRANSLATE_NOOP("USB", "Manual Screen Configuration"), TRANSLATE_NOOP("USB", "Forces the use of the screen parameters below, instead of automatic parameters if available."), "false"},
		};
		return info;
	}


	USBDevice* Python2Device::CreateDevice(SettingsInterface& si, u32 port, u32 subtype) const
	{
		Python2State* s = new Python2State(port);
		s->desc.full = &s->desc_dev;
		s->desc.str = &python2io_desc_strings[0];

		if (usb_desc_parse_dev(python2_dev_desc, sizeof(python2_dev_desc), s->desc, s->desc_dev) < 0)
			goto fail;
		if (usb_desc_parse_config(python2_config_desc, sizeof(python2_config_desc), s->desc_dev) < 0)
			goto fail;

		s->dev.speed = USB_SPEED_FULL;
		s->dev.klass.handle_attach = usb_desc_attach;
		s->dev.klass.handle_reset = usb_python2_handle_reset;
		s->dev.klass.handle_control = usb_python2_handle_control;
		s->dev.klass.handle_data = usb_python2_handle_data;
		s->dev.klass.unrealize = usb_python2_unrealize;
		s->dev.klass.usb_desc = &s->desc;
		s->dev.klass.product_desc = "";

		usb_desc_init(&s->dev);
		usb_ep_init(&s->dev);
		usb_python2_handle_reset(&s->dev);

		return &s->dev;
	fail:
		usb_python2_unrealize(&s->dev);
		return nullptr;
	}
	
	bool Python2Device::GetInputState(USBDevice* dev, u32 bind)
	{
		Python2State* s = USB_CONTAINER_OF(dev, Python2State, dev);

		const u64 mask = 1ull << static_cast<u64>(bind);

		return (s->buttonState & mask) > 0;
	}
}
