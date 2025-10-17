
#pragma once
#include "USB/deviceproxy.h"

namespace usb_python2
{
	enum : u32
	{
		BID_TEST = 0x1,
		BID_SERVICE,
		BID_COIN1,
		BID_COIN2,

		BID_DDR_P1_START,
		BID_DDR_P1_SELECT_LEFT,
		BID_DDR_P1_SELECT_RIGHT,
		BID_DDR_P1_FOOT_LEFT,
		BID_DDR_P1_FOOT_RIGHT,
		BID_DDR_P1_FOOT_UP,
		BID_DDR_P1_FOOT_DOWN,

		BID_DDR_P2_START,
		BID_DDR_P2_SELECT_LEFT,
		BID_DDR_P2_SELECT_RIGHT,
		BID_DDR_P2_FOOT_LEFT,
		BID_DDR_P2_FOOT_RIGHT,
		BID_DDR_P2_FOOT_UP,
		BID_DDR_P2_FOOT_DOWN,
				
		BID_KEYPADP1_0,
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
		BID_KEYPADP1_CARD_IN,
		
		BID_KEYPADP2_0,
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
		BID_KEYPADP2_CARD_IN,
	};
	
	class Python2Device final : public DeviceProxy
	{
	public:
		USBDevice* CreateDevice(SettingsInterface& si, u32 port, u32 subtype) const override;
		const char* Name() const override;
		const char* TypeName() const override;
		const char* IconName() const override;
		bool Freeze(USBDevice* dev, StateWrapper& sw) const override;
		void UpdateSettings(USBDevice* dev, SettingsInterface& si) const override;
		float GetBindingValue(const USBDevice* dev, u32 bind) const override;
		void SetBindingValue(USBDevice* dev, u32 bind, float value) const override;
		std::span<const InputBindingInfo> Bindings(u32 subtype) const override;
		std::span<const SettingInfo> Settings(u32 subtype) const override;

		static bool GetInputState(USBDevice* dev, u32 bind);
	};
}
