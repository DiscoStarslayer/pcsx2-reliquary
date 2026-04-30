#pragma once

#include "USB/usb-python2/devices/acio.h"
#include "USB/usb-python2/usb-python2.h"

namespace usb_python2
{
	class thrilldrive_belt_device final : public acio_device_base
	{
	public:
		explicit thrilldrive_belt_device(USBDevice* usbDevice);

		bool device_write(std::vector<uint8_t>& packet, std::vector<uint8_t>& outputResponse) override;

	private:
		void write(std::vector<uint8_t>& packet) override {}
		void update_seatbelt_state();

		USBDevice* usbDev = nullptr;
		bool seatBeltStatus = false;
		bool seatBeltButtonPressed = false;
	};
} // namespace usb_python2
