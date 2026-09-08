#include "ddr_extio.h"
#include "../ddr-hardware.h"

#include "common/Console.h"

namespace usb_python2
{
	extio_device::extio_device(DDRHardware& hardware)
		: m_hardware(hardware)
	{
	}

	// Reference: https://github.com/nchowning/open-io/blob/master/extio-emulator.ino
	void extio_device::write(std::vector<uint8_t>& packet)
	{
		if (!isOpen)
			return;

		if (packet.size() != 4)
			return;

#if PCSX2_DEVBUILD
		DevCon.WriteLn("EXTIO packet: %02x %02x %02x %02x", packet[0], packet[1], packet[2], packet[3]);
#endif

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

		m_hardware.SetExtioLights(packet[0], packet[1], packet[2]);

		std::vector<uint8_t> response;
		response.push_back(0x11);
		packet.erase(packet.begin(), packet.begin() + 4);

		add_packet(response);
	}
} // namespace usb_python2
