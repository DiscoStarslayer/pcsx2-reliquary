#include "PrecompiledHeader.h"

#include "thrilldrive_handle.h"

namespace usb_python2
{
	bool thrilldrive_handle_device::device_write(std::vector<uint8_t>& packet, std::vector<uint8_t>& outputResponse)
	{
		const auto header = reinterpret_cast<ACIO_PACKET_HEADER*>(packet.data());
		const auto code = BigEndian16(header->code);

		std::vector<uint8_t> response;
		if (code == 0x0002)
		{
			const uint8_t resp[] = {
				0x03, 0x00, 0x00, 0x00, // Device ID
				0x00, // Flag
				0x01, // Major version
				0x01, // Minor version
				0x00, // Version
				'H', 'N', 'D', 'L', // Product code
				'O', 'c', 't', ' ', '2', '6', ' ', '2', '0', '0', '5', '\0', '\0', '\0', '\0', '\0', // Date
				'1', '3', ' ', ':', ' ', '5', '5', ' ', ':', ' ', '0', '3', '\0', '\0', '\0', '\0' // Time
			};

			response.insert(response.end(), std::begin(resp), std::end(resp));
		}
		else if (code == 0x0100)
		{
			wheelForceFeedback = 0;
			wheelCalibrationHack = false;
			response.push_back(0);
		}
		else if (code == 0x0120)
		{
			const int8_t ffb1 = (packet.size() > 6) ? static_cast<int8_t>(packet[6]) : 0;
			const int8_t ffb4 = (packet.size() > 9) ? static_cast<int8_t>(packet[9]) : 0;
			wheelForceFeedback = ffb1;
			wheelCalibrationHack = (ffb4 == -2);

			const uint8_t resp[] = {0, 0, 0, 0};
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
