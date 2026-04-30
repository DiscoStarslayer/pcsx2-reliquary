#include "PrecompiledHeader.h"

#include "thrilldrive_belt.h"

namespace usb_python2
{
	thrilldrive_belt_device::thrilldrive_belt_device(USBDevice* usbDevice)
		: usbDev(usbDevice)
	{
	}

	void thrilldrive_belt_device::update_seatbelt_state()
	{
		const bool is_pressed = Python2Device::GetInputState(usbDev, BID_THRILLDRIVE_SEATBELT);
		if (is_pressed && !seatBeltButtonPressed)
			seatBeltStatus = !seatBeltStatus;

		seatBeltButtonPressed = is_pressed;
	}

	bool thrilldrive_belt_device::device_write(std::vector<uint8_t>& packet, std::vector<uint8_t>& outputResponse)
	{
		const auto header = reinterpret_cast<ACIO_PACKET_HEADER*>(packet.data());
		const auto code = BigEndian16(header->code);

		update_seatbelt_state();

		std::vector<uint8_t> response;
		if (code == 0x0002)
		{
			const uint8_t resp[] = {
				0x00, 0x00, 0x00, 0x00, // Device ID
				0x00, // Flag
				0x01, // Major version
				0x01, // Minor version
				0x00, // Version
				'B', 'E', 'L', 'T', // Product code
				'O', 'c', 't', ' ', '2', '6', ' ', '2', '0', '0', '5', '\0', '\0', '\0', '\0', '\0', // Date
				'1', '3', ' ', ':', ' ', '5', '5', ' ', ':', ' ', '0', '3', '\0', '\0', '\0', '\0' // Time
			};

			response.insert(response.end(), std::begin(resp), std::end(resp));
		}
		else if (code == 0x0100)
		{
			seatBeltStatus = false;
			seatBeltButtonPressed = false;
			response.push_back(0);
		}
		else if (code == 0x0102 || code == 0x0110)
		{
			response.push_back(0);
		}
		else if (code == 0x0111)
		{
			const uint8_t resp[] = {0, 0, 0, 0};
			response.insert(response.end(), std::begin(resp), std::end(resp));
		}
		else if (code == 0x0113)
		{
			const uint8_t resp[] = {0, 0, static_cast<uint8_t>(seatBeltStatus ? 0 : 0xff)};
			response.insert(response.end(), std::begin(resp), std::end(resp));
		}
		else
		{
			response.push_back(0);
		}

		outputResponse.insert(outputResponse.end(), response.begin(), response.end());
		return true;
	}
} // namespace usb_python2
