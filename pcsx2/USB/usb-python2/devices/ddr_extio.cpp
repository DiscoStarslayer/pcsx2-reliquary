#include "ddr_extio.h"

namespace usb_python2
{
	enum
	{
		EXTIO_LIGHT_PANEL_UP = 0x40,
		EXTIO_LIGHT_PANEL_DOWN = 0x20,
		EXTIO_LIGHT_PANEL_LEFT = 0x10,
		EXTIO_LIGHT_PANEL_RIGHT = 0x08,

		EXTIO_LIGHT_SENSOR_UP = 0x10,
		EXTIO_LIGHT_SENSOR_DOWN = 0x18,
		EXTIO_LIGHT_SENSOR_LEFT = 0x20,
		EXTIO_LIGHT_SENSOR_RIGHT = 0x28,
		EXTIO_LIGHT_SENSOR_ALL = 0x08,

		EXTIO_LIGHT_NEON = 0x40,
	};

	uint32_t oldLightPad1 = 0;
	uint32_t oldLightPad2 = 0;
	uint32_t oldLightBass = 0;
	uint32_t oldExtioState = 0;
	bool isMinimaidConnected = false;
	bool isUsingBtoolLights = false;

	extio_device::extio_device()
	{
	}

	// Reference: https://github.com/nchowning/open-io/blob/master/extio-emulator.ino
	void extio_device::write(std::vector<uint8_t>& packet)
	{
		if (!isOpen)
			return;

#if PCSX2_DEVBUILD
		printf("EXTIO packet: ");
		for (int i = 0; i < packet.size(); i++)
			printf("%02x ", packet[i]);
		printf("\n");
#endif

		if (packet.size() != 4)
			return;

		/*
		* DDR:
		* 80 00 40 40 CCFL
		* 90 00 00 10 1P FOOT LEFT
		* c0 00 00 40 1P FOOT UP
		* 88 00 00 08 1P FOOT RIGHT
		* a0 00 00 20 1P FOOT DOWN
		* 80 10 00 10 2P FOOT LEFT
		* 80 40 00 40 2P FOOT UP
		* 80 08 00 08 2P FOOT RIGHT
		* 80 20 00 20 2P FOOT DOWN
		*/

		const auto expectedChecksum = packet[3];
		const uint8_t calculatedChecksum = (packet[0] + packet[1] + packet[2]) & 0x7f;

		if (calculatedChecksum != expectedChecksum)
		{
			//printf("EXTIO packet checksum invalid! %02x vs %02x\n", expectedChecksum, calculatedChecksum);
			return;
		}

		std::vector<uint8_t> response;
		response.push_back(0x11);
		packet.erase(packet.begin(), packet.begin() + 4);

		add_packet(response);
	}
} // namespace usb_python2