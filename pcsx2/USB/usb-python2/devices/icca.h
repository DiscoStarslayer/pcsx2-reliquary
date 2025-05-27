#pragma once
#include "USB/usb-python2/usb-python2.h"
#include "acio.h"

namespace usb_python2
{
	class acio_icca_device : public acio_device_base
	{
	private:
		USBDevice* usbDev;
		
	protected:
		uint8_t keyLastActiveState = 0;
		uint8_t keyLastActiveEvent[2] = {0, 0};
		bool accept = false;
		bool inserted = false;
		bool isCardInsertPressed = false;
		bool isKeypadSwapped = false;
		bool isKeypadSwapPressed = false;

		bool cardLoaded = false;
		uint8_t cardId[8] = {0};
		std::string cardFilename = "";

		u32 keypadIdsByDeviceId[2][12] = {
			{BID_KEYPADP1_0,
				BID_KEYPADP1_1,
				BID_KEYPADP1_2,
				BID_KEYPADP1_3,
				BID_KEYPADP1_4,
				BID_KEYPADP1_5,
				BID_KEYPADP1_6,
				BID_KEYPADP1_7,
				BID_KEYPADP1_8,
				BID_KEYPADP1_9,
				BID_KEYPADP1_00,
				BID_KEYPADP1_CARD_IN},
			{BID_KEYPADP2_0,
				BID_KEYPADP2_1,
				BID_KEYPADP2_2,
				BID_KEYPADP2_3,
				BID_KEYPADP2_4,
				BID_KEYPADP2_5,
				BID_KEYPADP2_6,
				BID_KEYPADP2_7,
				BID_KEYPADP2_8,
				BID_KEYPADP2_9,
				BID_KEYPADP2_00,
				BID_KEYPADP2_CARD_IN},
		};

		void write(std::vector<uint8_t>& packet) {}

	public:
		acio_icca_device(USBDevice* usbDevice)
		{
			usbDev = usbDevice;
		}

		acio_icca_device(USBDevice* usbDevice, std::string targetCardFilename)
		{
			usbDev = usbDevice;
			cardFilename = targetCardFilename;
		}

		bool device_write(std::vector<uint8_t>& packet, std::vector<uint8_t>& outputResponse);
	};
} // namespace usb_python2
#pragma once