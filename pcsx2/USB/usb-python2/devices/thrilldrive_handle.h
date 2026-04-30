#pragma once

#include "USB/usb-python2/devices/acio.h"

namespace usb_python2
{
	class thrilldrive_handle_device final : public acio_device_base
	{
	public:
		bool device_write(std::vector<uint8_t>& packet, std::vector<uint8_t>& outputResponse) override;

	private:
		void write(std::vector<uint8_t>& packet) override {}

		int8_t wheelForceFeedback = 0;
		bool wheelCalibrationHack = false;
	};
} // namespace usb_python2
