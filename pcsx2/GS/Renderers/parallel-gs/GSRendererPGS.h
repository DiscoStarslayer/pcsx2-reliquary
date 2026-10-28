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
#include "analog_video.hpp"
#include "GS/Renderers/Common/GSTexture.h"

// Purely for interop with fullscreen UI.
class GSTexturePGS final : public GSTexture
{
public:
	void* GetNativeHandle() const override { return const_cast<Vulkan::Image*>(img.get()); }

	bool Update(const GSVector4i&, const void*, int, int) override
	{
		return false;
	}

	bool Map(GSMap&, const GSVector4i*, int) override
	{
		return false;
	}

	void Unmap() override {}
	void GenerateMipmap() override {}

#ifdef PCSX2_DEVBUILD
	void SetDebugName(std::string_view) override {}
#endif

	explicit GSTexturePGS(Vulkan::ImageHandle img_)
		: img(std::move(img_))
	{
		m_size.x = img->get_width();
		m_size.y = img->get_height();
		m_mipmap_levels = 1;
		m_usage = Usage::Texture;
		m_format = Format::Color;
	}

private:
	Vulkan::ImageHandle img;
};

// Somewhat stilted split, but it's necessary to integrate with UI code in a somewhat reasonable way.
class GSDevicePGS final : private Vulkan::WSIPlatform
{
public:
	friend class GSRendererPGS;
	bool Init();
	~GSDevicePGS();
	Vulkan::WSI& get_wsi() { return wsi; }
	Vulkan::Device& get_device() { return wsi.get_device(); }

	void DestroyImGuiTextures();
	GSTexture* CreateTexture(u32 width, u32 height, const void* pixels, u32 pitch);

	void ResizeWindow(int width, int height, float scale);
	const WindowInfo& GetWindowInfo() const;
	u32 GetWindowWidth() const { return window_info.surface_width; }
	u32 GetWindowHeight() const { return window_info.surface_height; }
	float GetWindowScale() const { return window_info.surface_scale; }
	void SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle);

private:
	Vulkan::WSI wsi;

	bool UpdateWindow();
	VkSurfaceKHR create_surface(VkInstance instance, VkPhysicalDevice gpu) override;
	void destroy_surface(VkInstance instance, VkSurfaceKHR surface) override;
	std::vector<const char*> get_instance_extensions() override;
	std::vector<const char*> get_device_extensions() override;
	bool alive(Vulkan::WSI& wsi) override;
	uint32_t get_surface_width() override;
	uint32_t get_surface_height() override;
	void poll_input() override;
	void poll_input_async(Granite::InputTrackerHandler*) override;
	void event_swapchain_destroyed() override;
	const VkApplicationInfo* get_application_info() override;

	WindowInfo window_info = {};
	bool has_wsi_begin_frame = false;
	bool has_presented_in_current_swapchain = false;
};

class GSRendererPGS
{
public:
	GSRendererPGS(GSDevicePGS& device, u8* basemem);

	bool Init();
	bool UpdateWindow();

	void Reset(bool hardware_reset);

	void Transfer(const u8* mem, u32 size);

	void VSync(u32 field, bool registers_written, bool refresh_frame);

	inline ParallelGS::GSInterface& get_interface() { return iface; };
	void ReadFIFO(u8* mem, u32 size);

	void UpdateConfig();

	void GetInternalResolution(int* width, int* height);
	bool IsProgressive() const { return priv->smode1.CMOD == 0; }
	GSVector2i GetPresentationSourceSize() const { return last_presentation_size; }
	float GetDisplayAspectRatio() const { return last_display_aspect_ratio; }
	const ParallelGS::FlushStats& GetLastFrameStats() const { return last_frame_stats; }

	int Freeze(freezeData* data, bool sizeonly);
	int Defrost(freezeData* data);

	u8* GetRegsMem();

	void QueueSnapshot(const std::string& path, u32 gsdump_frames);
	void StopGSDump();

private:
	GSDevicePGS& device;
	ParallelGS::PrivRegisterState* priv;
	ParallelGS::GSInterface iface;

	Vulkan::Program* upscale_program = nullptr;
	Vulkan::Program* sharpen_program = nullptr;
	Vulkan::Program* blit_program = nullptr;
	Vulkan::Program* ui_program[2][2] = {};
	void render_fsr(Vulkan::CommandBuffer& cmd, const Vulkan::ImageView& view);
	void render_rcas(Vulkan::CommandBuffer& cmd, const Vulkan::ImageView& view,
		float offset_x, float offset_y,
		float width, float height);
	void render_blit(Vulkan::CommandBuffer& cmd, const Vulkan::ImageView& view,
		float offset_x, float offset_y,
		float width, float height);
	bool SetGPUTimingEnabled(bool enabled);
	float GetAndResetAccumulatedGPUTime();
	struct GPUTimestamp
	{
		Vulkan::QueryPoolHandle start;
		Vulkan::QueryPoolHandle end;
	};
	Vulkan::ImageHandle fsr_render_target;
	ParallelGS::ScanoutResult vsync;

	ParallelGS::SuperSampling current_super_sampling = ParallelGS::SuperSampling::X1;
	ParallelGS::FlushStats last_frame_stats = {};
	bool current_ordered_super_sampling = false;
	bool current_super_sample_textures = false;
	uint32_t last_internal_width = 0;
	uint32_t last_internal_height = 0;
	GSVector2i last_presentation_size{0, 0};
	float last_display_aspect_ratio = 0.0f;
	double last_gpu_timestamp = 0.0;
	bool gpu_timing_enabled = false;
	std::vector<GPUTimestamp> gpu_timestamps;

	static int GetSaveStateSize(int version);

	bool QueueImageReadback(Vulkan::CommandBuffer& cmd, const Vulkan::Image& image,
		Vulkan::BufferHandle* readback, u32 width, u32 height, bool* bgra);
	bool SaveScreenshotReadback(const Vulkan::Buffer& readback, u32 width, u32 height, bool bgra);
	void QueueGSDump(const std::string& path, u32 gsdump_frames);

	std::string m_snapshot;
	std::unique_ptr<GSDumpBase> dump;
	uint32_t dump_frames = 0;

	ParallelGS::AnalogVideoFilter analog_filter;
	ParallelGS::CRTFilter crt_filter;

	void render_ui_prepare(Vulkan::CommandBuffer& cmd);
	void render_ui_flush(Vulkan::CommandBuffer& cmd);
	void render_ui_end();
};
