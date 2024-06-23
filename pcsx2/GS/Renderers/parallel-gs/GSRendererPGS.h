// SPDX-FileCopyrightText: 2024 Hans-Kristian Arntzen
// SPDX-License-Identifier: LGPL-3.0+

#pragma once

#include "../Vulkan/VKLoaderPlatformDefines.h"
#include "SaveState.h"
#include "GS/GSDump.h"
#include "Config.h"
#include "common/WindowInfo.h"
#include "gs_interface.hpp"
#include "device.hpp"
#include "context.hpp"
#include "wsi.hpp"

class GSRendererPGS final : private Vulkan::WSIPlatform
{
public:
	explicit GSRendererPGS(u8 *basemem);

	bool Init();
	bool UpdateWindow();
	void ResizeWindow(int width, int height, float scale);
	const WindowInfo &GetWindowInfo() const;
	void SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle);
	void Reset(bool hardware_reset);

	void Transfer(const u8 *mem, u32 size);

	void VSync(u32 field, bool registers_written);
	inline ParallelGS::GSInterface &get_interface() { return iface; };
	void ReadFIFO(u8 *mem, u32 size);

	void UpdateConfig();

	void GetInternalResolution(int *width, int *height);

	int Freeze(freezeData *data, bool sizeonly);
	int Defrost(freezeData *data);

	u8 *GetRegsMem();

	void QueueSnapshot(const std::string &path, u32 gsdump_frames);

private:
	VkSurfaceKHR create_surface(VkInstance instance, VkPhysicalDevice gpu) override;
	void destroy_surface(VkInstance instance, VkSurfaceKHR surface) override;
	std::vector<const char *> get_instance_extensions() override;
	std::vector<const char *> get_device_extensions() override;
	bool alive(Vulkan::WSI &wsi) override;
	uint32_t get_surface_width() override;
	uint32_t get_surface_height() override;
	void poll_input() override;
	void poll_input_async(Granite::InputTrackerHandler *) override;

	ParallelGS::PrivRegisterState *priv;
	Vulkan::WSI wsi;
	ParallelGS::GSInterface iface;
	WindowInfo window_info = {};
	bool has_wsi_begin_frame = false;

	Vulkan::Program *upscale_program = nullptr;
	Vulkan::Program *sharpen_program = nullptr;
	Vulkan::Program *blit_program = nullptr;
	void render_fsr(Vulkan::CommandBuffer &cmd, const Vulkan::ImageView &view);
	void render_rcas(Vulkan::CommandBuffer &cmd, const Vulkan::ImageView &view,
	                 float offset_x, float offset_y,
	                 float width, float height);
	void render_blit(Vulkan::CommandBuffer &cmd, const Vulkan::ImageView &view,
	                 float offset_x, float offset_y,
	                 float width, float height);
	void event_swapchain_destroyed() override;
	Vulkan::ImageHandle fsr_render_target;
	ParallelGS::ScanoutResult vsync;

	ParallelGS::SuperSampling current_super_sampling = ParallelGS::SuperSampling::X1;
	bool current_ordered_super_sampling = false;
	bool current_super_sample_textures = false;
	bool has_presented_in_current_swapchain = false;
	uint32_t last_internal_width = 0;
	uint32_t last_internal_height = 0;

	static int GetSaveStateSize(int version);
	const VkApplicationInfo* get_application_info() override;

	std::unique_ptr<GSDumpBase> dump;
	uint32_t dump_frames = 0;
};
