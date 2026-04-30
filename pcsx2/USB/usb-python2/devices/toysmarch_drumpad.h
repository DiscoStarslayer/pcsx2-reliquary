#pragma once

#include "USB/usb-python2/devices/input_device.h"
#include "USB/usb-python2/usb-python2.h"

namespace usb_python2
{
	class toysmarch_drumpad_device final : public input_device
	{
	public:
		explicit toysmarch_drumpad_device(USBDevice* usbDevice);

		int read(std::vector<uint8_t>& buf, const size_t requestedLen) override;
		void write(std::vector<uint8_t>& packet) override;

	private:
		USBDevice* usbDev = nullptr;
		uint8_t stateIndex = 0;
		bool lastInputState[6] = {};

		uint8_t get_oneshot_state(u32 binding, size_t stateIndex);
	};
} // namespace usb_python2
