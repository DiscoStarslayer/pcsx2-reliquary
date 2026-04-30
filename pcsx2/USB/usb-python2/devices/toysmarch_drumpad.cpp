#include "PrecompiledHeader.h"

#include "toysmarch_drumpad.h"

#include <array>
#include <numeric>

namespace usb_python2
{
	toysmarch_drumpad_device::toysmarch_drumpad_device(USBDevice* usbDevice)
		: usbDev(usbDevice)
	{
	}

	uint8_t toysmarch_drumpad_device::get_oneshot_state(u32 binding, size_t index)
	{
		const bool is_pressed = Python2Device::GetInputState(usbDev, binding);
		const bool was_pressed = lastInputState[index];
		lastInputState[index] = is_pressed;
		return (is_pressed && !was_pressed) ? 0x80 : 0;
	}

	int toysmarch_drumpad_device::read(std::vector<uint8_t>& buf, const size_t requestedLen)
	{
		std::array<uint8_t, 9> state = {
			stateIndex,
			get_oneshot_state(BID_TOYSMARCH_P1_CYMBAL, 0),
			get_oneshot_state(BID_TOYSMARCH_P1_DRUM_L, 1),
			get_oneshot_state(BID_TOYSMARCH_P1_DRUM_R, 2),
			0,
			get_oneshot_state(BID_TOYSMARCH_P2_CYMBAL, 3),
			get_oneshot_state(BID_TOYSMARCH_P2_DRUM_L, 4),
			get_oneshot_state(BID_TOYSMARCH_P2_DRUM_R, 5),
			0,
		};

		stateIndex = (stateIndex + 1) % 8;

		std::vector<uint8_t> response;
		response.reserve(state.size() + 2);
		response.push_back(0xaa);
		response.insert(response.end(), state.begin(), state.end());
		response.push_back(std::accumulate(state.begin(), state.end(), 0));

		buf.insert(buf.end(), response.begin(), response.end());
		return static_cast<int>(response.size());
	}

	void toysmarch_drumpad_device::write(std::vector<uint8_t>& packet)
	{
		// Toy's March sends periodic keep-alive packets and reads the pad state independently.
	}
} // namespace usb_python2
