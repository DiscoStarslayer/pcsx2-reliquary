#pragma once
#include "input_device.h"

namespace usb_python2
{
	class DDRHardware;

	class extio_device : public input_device
	{
		DDRHardware& m_hardware;
		void write(std::vector<uint8_t>& packet);

	public:
		explicit extio_device(DDRHardware& hardware);
	};
} // namespace usb_python2
#pragma once