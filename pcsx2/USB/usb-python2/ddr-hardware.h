// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstdint>
#include <memory>

namespace usb_python2
{
	class DDRHardware final
	{
	public:
		DDRHardware();
		~DDRHardware();
		DDRHardware(const DDRHardware&) = delete;
		DDRHardware& operator=(const DDRHardware&) = delete;

		void Configure(bool ddrio, bool minimaid);
		void ResetLights();
		void SetCabinetLights(uint8_t lamps);
		void SetExtioLights(uint8_t p1, uint8_t p2, uint8_t neon);
		uint32_t GetPressedButtons() const;

	protected:
		static constexpr uint32_t DDRIOToJamma(uint32_t pad)
		{
			return ((pad & 0x00df0000) >> 8) | ((pad & 0x0000df00) << 8) | ((pad & 0x70) << 24);
		}

		static constexpr uint32_t DDRCabinetLights(uint8_t lamps)
		{
			return (~lamps) & 0xf3;
		}

		static constexpr uint32_t DDRExtioLights(uint8_t p1, uint8_t p2, uint8_t neon)
		{
			return (uint32_t(p1 & 0x78) << 24) | (uint32_t(p2 & 0x78) << 16) | (uint32_t(neon & 0x40) << 8);
		}

#ifdef _WIN32
		void ConfigureDDRIO(bool enabled);
		void ConfigureMinimaid(bool enabled);
#endif

	private:
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};
} // namespace usb_python2
