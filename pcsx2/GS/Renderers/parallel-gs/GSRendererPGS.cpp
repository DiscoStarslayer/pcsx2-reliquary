// SPDX-FileCopyrightText: 2024 Hans-Kristian Arntzen
// SPDX-License-Identifier: LGPL-3.0+

#include "GSRendererPGS.h"
#include "GS/GSScreenshot.h"
#include "GS/GSState.h"
#include "GSDumpReplayer.h"
#include "GS.h"
#include "Host.h"
#include "math.hpp"
#include "muglm/muglm_impl.hpp"
#include "shaders/slangmosh.hpp"
#include "PerformanceMetrics.h"
#include "VMManager.h"
#include "command_buffer.hpp"
#include "ImGui/FullscreenUI.h"
#include "ImGui/ImGuiManager.h"
#include "IconsFontAwesome.h"
#include "imgui.h"

#include "common/StringUtil.h"
#include "common/Console.h"

// Workaround because msbuild is broken when mixing C and C++ it seems ...
#ifdef _MSC_VER
extern "C" {
#include "volk.c"
}
#endif

using namespace Vulkan;
using namespace ParallelGS;
using namespace Granite;

static void FsrEasuCon(
	float *con0,
	float *con1,
	float *con2,
	float *con3,
	float inputViewportInPixelsX,
	float inputViewportInPixelsY,
	float inputSizeInPixelsX,
	float inputSizeInPixelsY,
	float outputSizeInPixelsX,
	float outputSizeInPixelsY)
{
	// Output integer position to a pixel position in viewport.
	con0[0] = inputViewportInPixelsX / outputSizeInPixelsX;
	con0[1] = inputViewportInPixelsY / outputSizeInPixelsY;
	con0[2] = 0.5f * inputViewportInPixelsX / outputSizeInPixelsX - 0.5f;
	con0[3] = 0.5f * inputViewportInPixelsY / outputSizeInPixelsY - 0.5f;
	con1[0] = 1.0f / inputSizeInPixelsX;
	con1[1] = 1.0f / inputSizeInPixelsY;
	con1[2] = 1.0f / inputSizeInPixelsX;
	con1[3] = -1.0f / inputSizeInPixelsY;
	con2[0] = -1.0f / inputSizeInPixelsX;
	con2[1] = 2.0f / inputSizeInPixelsY;
	con2[2] = 1.0f / inputSizeInPixelsX;
	con2[3] = 2.0f / inputSizeInPixelsY;
	con3[0] = 0.0f / inputSizeInPixelsX;
	con3[1] = 4.0f / inputSizeInPixelsY;
	con3[2] = con3[3] = 0.0f;
}

static void FsrRcasCon(float *con, float sharpness)
{
	sharpness = muglm::exp2(-sharpness);
	uint32_t half = floatToHalf(sharpness);
	con[0] = sharpness;
	uint32_t halves = half | (half << 16);
	memcpy(&con[1], &halves, sizeof(halves));
	con[2] = 0.0f;
	con[3] = 0.0f;
}

void GSRendererPGS::render_fsr(CommandBuffer &cmd, const ImageView &view)
{
	cmd.image_barrier(*fsr_render_target,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

	RenderPassInfo rp = {};
	rp.num_color_attachments = 1;
	rp.color_attachments[0] = &fsr_render_target->get_view();
	rp.store_attachments = 1u << 0;

	cmd.begin_render_pass(rp);
	{
		struct Constants
		{
			float params[4][4];
		} constants;


		auto width = float(view.get_image().get_width());
		auto height = float(view.get_image().get_height());
		auto *params = cmd.allocate_typed_constant_data<Constants>(1, 0, 1);
		FsrEasuCon(constants.params[0], constants.params[1], constants.params[2], constants.params[3],
			width, height, width, height, cmd.get_viewport().width, cmd.get_viewport().height);
		*params = constants;

		struct Push
		{
			float width, height;
		} push;

		push.width = cmd.get_viewport().width;
		push.height = cmd.get_viewport().height;
		cmd.push_constants(&push, 0, sizeof(push));

		const vec2 vertex_data[] = { vec2(-1.0f, -1.0f), vec2(-1.0f, 3.0f), vec2(3.0f, -1.0f) };
		memcpy(cmd.allocate_vertex_data(0, sizeof(vertex_data), sizeof(vec2)), vertex_data, sizeof(vertex_data));
		cmd.set_vertex_attrib(0, 0, VK_FORMAT_R32G32_SFLOAT, 0);

		cmd.set_texture(0, 0, view, StockSampler::NearestClamp);

		cmd.set_program(upscale_program);
		cmd.set_opaque_state();
		cmd.set_depth_test(false, false);
		cmd.draw(3);
	}
	cmd.end_render_pass();

	cmd.image_barrier(*fsr_render_target,
		VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void GSRendererPGS::render_rcas(CommandBuffer &cmd, const ImageView &view,
                                float offset_x, float offset_y,
                                float width, float height)
{
	struct Constants
	{
		float params[4];
		int32_t range[4];
	} constants;

	FsrRcasCon(constants.params, 0.5f);
	constants.range[0] = 0;
	constants.range[1] = 0;
	constants.range[2] = int(view.get_view_width()) - 1;
	constants.range[3] = int(view.get_view_height()) - 1;
	auto *params = cmd.allocate_typed_constant_data<Constants>(1, 0, 1);
	*params = constants;

	const vec2 vertex_data[] = { vec2(-1.0f, -1.0f), vec2(-1.0f, 3.0f), vec2(3.0f, -1.0f) };
	memcpy(cmd.allocate_vertex_data(0, sizeof(vertex_data), sizeof(vec2)), vertex_data, sizeof(vertex_data));
	cmd.set_vertex_attrib(0, 0, VK_FORMAT_R32G32_SFLOAT, 0);
	cmd.set_srgb_texture(0, 0, view);
	cmd.set_sampler(0, 0, StockSampler::NearestClamp);
	cmd.set_opaque_state();
	cmd.set_depth_test(false, false);
	cmd.set_program(sharpen_program);

	struct Push
	{
		float width, height;
	} push;

	push.width = width;
	push.height = height;
	cmd.push_constants(&push, 0, sizeof(push));

	cmd.set_viewport({ offset_x, offset_y, width, height, 0.0f, 1.0f });
	cmd.draw(3);
}

void GSRendererPGS::render_blit(CommandBuffer &cmd, const ImageView &view,
                                float offset_x, float offset_y,
                                float width, float height)
{
	cmd.set_srgb_texture(0, 0, view);
	cmd.set_sampler(0, 0, GSConfig.LinearPresent != GSPostBilinearMode::Off ?
	                      StockSampler::LinearClamp : StockSampler::NearestClamp);
	cmd.set_opaque_state();
	cmd.set_depth_test(false, false);
	cmd.set_program(blit_program);

	cmd.set_viewport({ offset_x, offset_y, width, height, 0.0f, 1.0f });
	cmd.draw(3);
}

GSRendererPGS::GSRendererPGS(GSDevicePGS &device_, u8 *basemem)
	: device(device_), priv(reinterpret_cast<PrivRegisterState *>(basemem))
{
	device.get_wsi().set_backbuffer_format(BackbufferFormat::sRGB);
}

u8 *GSRendererPGS::GetRegsMem()
{
	return reinterpret_cast<u8 *>(priv);
}

struct ParsedSuperSampling
{
	SuperSampling super_sampling;
	bool ordered;
};

static ParsedSuperSampling parse_super_sampling_options(u8 super_sampling)
{
	ParsedSuperSampling parsed = {};

	if (super_sampling > 5)
		super_sampling = 5;

	parsed.ordered = super_sampling == 3;

	if (super_sampling >= 3)
		super_sampling--;
	parsed.super_sampling = SuperSampling(1u << super_sampling);

	return parsed;
}

bool GSDevicePGS::Init()
{
	// Always force the reload, since the other backends may clobber the volk pointers.
	if (!Context::init_loader(nullptr, true))
		return false;

	wsi.set_platform(this);
	wsi.set_frame_duplication_aware(true, 5);

	bool ret = wsi.init_simple(1, {});
	if (!ret)
		return false;

	// We will cycle through many memory contexts per frame most likely.
	wsi.get_device().init_frame_contexts(12);

	return true;
}

bool GSRendererPGS::Init()
{
	auto &wsi = device.get_wsi();

	ResourceLayout layout;
	Shaders<> suite(wsi.get_device(), layout, 0);
	upscale_program = wsi.get_device().request_program(suite.upscale_vert, suite.upscale_frag);
	sharpen_program = wsi.get_device().request_program(suite.sharpen_vert, suite.sharpen_frag);
	blit_program = wsi.get_device().request_program(suite.quad, suite.blit);
	ui_program[0][0] = wsi.get_device().request_program(suite.ui_vert, suite.ui_frag[0][0]);
	ui_program[0][1] = wsi.get_device().request_program(suite.ui_vert, suite.ui_frag[0][1]);
	ui_program[1][0] = wsi.get_device().request_program(suite.ui_vert, suite.ui_frag[1][0]);
	ui_program[1][1] = wsi.get_device().request_program(suite.ui_vert, suite.ui_frag[1][1]);

	GSOptions opts = {};
	opts.vram_size = GSLocalMemory::m_vmsize;

	auto parsed = parse_super_sampling_options(GSConfig.PGSSuperSampling);
	opts.super_sampling = parsed.super_sampling;
	opts.ordered_super_sampling = parsed.ordered;

	opts.dynamic_super_sampling = true;
	opts.super_sampled_textures = GSConfig.PGSSuperSampleTextures != 0;
	if (!iface.init(&wsi.get_device(), opts))
		return false;
	if (!SetGPUTimingEnabled(true))
		GSConfig.OsdShowGPU = false;

	current_super_sampling = opts.super_sampling;
	current_ordered_super_sampling = opts.ordered_super_sampling;
	current_super_sample_textures = opts.super_sampled_textures;

	Hacks hacks;
	hacks.disable_mipmaps = GSConfig.PGSDisableMipmaps != 0;
	hacks.unsynced_readbacks = GSConfig.PGSDisableReadbackSync != 0;
	hacks.backbuffer_promotion = GSConfig.PGSSharpBackbuffer != 0;
	hacks.allow_blend_demote = GSConfig.PGSBlendDemotion != 0;
	iface.set_hacks(hacks);

	if (!analog_filter.init(wsi.get_device(), {}))
		return false;
	if (!crt_filter.init(wsi.get_device()))
		return false;

	return true;
}

bool GSRendererPGS::SetGPUTimingEnabled(bool enabled)
{
	const auto &dev = device.get_device();
	const auto &props = dev.get_gpu_properties();
	const bool supported = props.limits.timestampComputeAndGraphics != 0 &&
	                       dev.get_queue_info().timestamp_valid_bits > 0 && props.limits.timestampPeriod > 0.0f;
	if (enabled && !supported)
		return false;

	DebugMode debug_mode = {};
	debug_mode.timestamps = enabled;
	iface.set_debug_mode(debug_mode);
	gpu_timing_enabled = enabled;

	last_gpu_timestamp = 0.0;
	for (int i = 0; i < static_cast<int>(TimestampType::Count); i++)
		last_gpu_timestamp += iface.get_accumulated_timestamps(static_cast<TimestampType>(i));

	return true;
}

float GSRendererPGS::GetAndResetAccumulatedGPUTime()
{
	if (!gpu_timing_enabled)
		return 0.0f;

	double gpu_timestamp = 0.0;
	for (int i = 0; i < static_cast<int>(TimestampType::Count); i++)
		gpu_timestamp += iface.get_accumulated_timestamps(static_cast<TimestampType>(i));

	double gpu_time = std::max(gpu_timestamp - last_gpu_timestamp, 0.0);
	last_gpu_timestamp = gpu_timestamp;

	auto timestamp_it = gpu_timestamps.begin();
	for (; timestamp_it != gpu_timestamps.end() && timestamp_it->start->is_signalled() &&
	       timestamp_it->end->is_signalled();
	     timestamp_it++)
	{
		gpu_time += device.get_device().convert_device_timestamp_delta(timestamp_it->start->get_timestamp_ticks(),
		                                                               timestamp_it->end->get_timestamp_ticks());
	}
	gpu_timestamps.erase(gpu_timestamps.begin(), timestamp_it);

	return static_cast<float>(gpu_time * 1000.0);
}

void GSRendererPGS::Reset(bool /*hardware_reset*/)
{
	iface.reset_context_state();
}

enum PGSDisplayCalibration
{
	PGS_DISPLAY_SDR,
	PGS_DISPLAY_PQ_400,
	PGS_DISPLAY_PQ_600,
	PGS_DISPLAY_PQ_1000,
	PGS_DISPLAY_PQ_FULL
};

static float display_calibration_to_max_cll(PGSDisplayCalibration calibration)
{
	switch (calibration)
	{
		case PGS_DISPLAY_PQ_400: return 400.0f;
		case PGS_DISPLAY_PQ_600: return 600.0f;
		case PGS_DISPLAY_PQ_1000: return 1000.0f;
		case PGS_DISPLAY_PQ_FULL: return 10000.0f;
		default: return 0.0f;
	}
}

void GSRendererPGS::UpdateConfig()
{
	auto parsed = parse_super_sampling_options(GSConfig.PGSSuperSampling);

	if (parsed.super_sampling != current_super_sampling || parsed.ordered != current_ordered_super_sampling ||
	    current_super_sample_textures != bool(GSConfig.PGSSuperSampleTextures))
	{
		iface.set_super_sampling_rate(parsed.super_sampling, parsed.ordered, GSConfig.PGSSuperSampleTextures != 0);
		current_super_sampling = parsed.super_sampling;
		current_ordered_super_sampling = parsed.ordered;
		current_super_sample_textures = GSConfig.PGSSuperSampleTextures != 0;
	}

	Hacks hacks;
	hacks.disable_mipmaps = GSConfig.PGSDisableMipmaps != 0;
	hacks.unsynced_readbacks = GSConfig.PGSDisableReadbackSync != 0;
	hacks.backbuffer_promotion = GSConfig.PGSSharpBackbuffer != 0;
	hacks.allow_blend_demote = GSConfig.PGSBlendDemotion != 0;
	iface.set_hacks(hacks);

	// 0 values mean we don't know / don't care.
	// On at least KDE, MaxCLL is used to guide the tonemapper.
	// If we specify a specific MaxCLL, it assumes a pure clip at that level.
	// The other metadata values are not known on CPU timeline in any meaningful way and are kinda irrelevant.
	VkHdrMetadataEXT meta = { VK_STRUCTURE_TYPE_HDR_METADATA_EXT };
	meta.maxContentLightLevel = display_calibration_to_max_cll(PGSDisplayCalibration(GSConfig.PGSDisplayCalibration));
	if (meta.maxContentLightLevel >= 10000.0f)
		meta.maxContentLightLevel = 0.0f;

	if (device.get_device().get_device_features().driver_id == VK_DRIVER_ID_NVIDIA_PROPRIETARY)
	{
		// NV workaround. It is out of spec and does not sanitize HDR metadata.
		meta.maxLuminance = meta.maxContentLightLevel;

		if (meta.maxLuminance == 0.0f)
		{
			meta.maxLuminance = 1000.0f;
			meta.maxContentLightLevel = 1000.0f;
		}
	}

	device.get_wsi().set_hdr_metadata(meta);
}

int GSRendererPGS::GetSaveStateSize(int version)
{
	return GSState::GetSaveStateSize(version);
}

static void write_data(u8*& dst, const void* src, size_t size)
{
	memcpy(dst, src, size);
	dst += size;
}

template <typename T>
static void write_reg(u8*& data, T t)
{
	write_data(data, &t, sizeof(t));
}

static void read_data(const u8*& src, void* dst, size_t size)
{
	memcpy(dst, src, size);
	src += size;
}

template <typename T>
static void read_reg(const u8*& src, T &t)
{
	read_data(src, &t, sizeof(t));
}

int GSRendererPGS::Freeze(freezeData* data, bool sizeonly)
{
	constexpr uint32_t version = 8; // v9 doesn't add anything meaningful for us.
	if (sizeonly)
	{
		data->size = GetSaveStateSize(version);
		return 0;
	}

	if (!data->data || data->size < GetSaveStateSize(version))
		return -1;

	const void *vram = iface.map_vram_read(0, GSLocalMemory::m_vmsize);
	auto &regs = iface.get_register_state();

	u8 *ptr = data->data;

	write_reg(ptr, version);
	write_reg(ptr, regs.prim);
	write_reg(ptr, regs.prmodecont);
	write_reg(ptr, regs.texclut);
	write_reg(ptr, regs.scanmsk);
	write_reg(ptr, regs.texa);
	write_reg(ptr, regs.fogcol);
	write_reg(ptr, regs.dimx);
	write_reg(ptr, regs.dthe);
	write_reg(ptr, regs.colclamp);
	write_reg(ptr, regs.pabe);
	write_reg(ptr, regs.bitbltbuf);
	write_reg(ptr, regs.trxdir);
	write_reg(ptr, regs.trxpos);
	write_reg(ptr, regs.trxreg);
	write_reg(ptr, regs.trxreg); // Dummy value

	for (const auto &ctx : regs.ctx)
	{
		write_reg(ptr, ctx.xyoffset);
		write_reg(ptr, ctx.tex0);
		write_reg(ptr, ctx.tex1);
		write_reg(ptr, ctx.clamp);
		write_reg(ptr, ctx.miptbl_1_3);
		write_reg(ptr, ctx.miptbl_4_6);
		write_reg(ptr, ctx.scissor);
		write_reg(ptr, ctx.alpha);
		write_reg(ptr, ctx.test);
		write_reg(ptr, ctx.fba);
		write_reg(ptr, ctx.frame);
		write_reg(ptr, ctx.zbuf);
	}

	write_reg(ptr, regs.rgbaq);
	write_reg(ptr, regs.st);
	write_reg(ptr, regs.uv.words[0]);
	write_reg(ptr, regs.fog.words[0]);
	// XYZ register, fill with dummy.
	write_reg(ptr, Reg64<XYZBits>{0});

	write_reg(ptr, UINT32_MAX); // Dummy GIFReg
	write_reg(ptr, UINT32_MAX);

	// Dummy transfer X/Y
	write_reg(ptr, uint32_t(0));
	write_reg(ptr, uint32_t(0));

	// v9 adds a lot more dummy stuff here which we don't care about

	write_data(ptr, vram, GSLocalMemory::m_vmsize);

	// 4 GIF paths
	for (int i = 0; i < 4; i++)
	{
		auto gif_path = iface.get_gif_path(i);
		gif_path.tag.NLOOP -= gif_path.loop;
		write_data(ptr, &gif_path.tag, sizeof(gif_path.tag));
		write_reg(ptr, gif_path.reg);
	}

	// internal_Q
	write_reg(ptr, regs.internal_q);
	return 0;
}

int GSRendererPGS::Defrost(freezeData* data)
{
	constexpr uint32_t expected_version = 8; // v9 doesn't add anything meaningful for us.

	if (!data || !data->data || data->size == 0)
		return -1;

	if (data->size < GetSaveStateSize(expected_version))
		return -1;

	const u8* ptr = data->data;
	auto &regs = iface.get_register_state();

	u32 version;
	read_reg(ptr, version);

	if (version != expected_version && version != GSState::STATE_VERSION)
	{
		Console.Error("GS: Savestate version is incompatible.  Load aborted.");
		return -1;
	}

	read_reg(ptr, regs.prim);
	read_reg(ptr, regs.prmodecont);
	read_reg(ptr, regs.texclut);
	read_reg(ptr, regs.scanmsk);
	read_reg(ptr, regs.texa);
	read_reg(ptr, regs.fogcol);
	read_reg(ptr, regs.dimx);
	read_reg(ptr, regs.dthe);
	read_reg(ptr, regs.colclamp);
	read_reg(ptr, regs.pabe);
	read_reg(ptr, regs.bitbltbuf);
	read_reg(ptr, regs.trxdir);
	read_reg(ptr, regs.trxpos);
	read_reg(ptr, regs.trxreg);
	// Dummy value
	ptr += sizeof(uint64_t);

	for (auto &ctx : regs.ctx)
	{
		read_reg(ptr, ctx.xyoffset);
		read_reg(ptr, ctx.tex0);
		read_reg(ptr, ctx.tex1);
		read_reg(ptr, ctx.clamp);
		read_reg(ptr, ctx.miptbl_1_3);
		read_reg(ptr, ctx.miptbl_4_6);
		read_reg(ptr, ctx.scissor);
		read_reg(ptr, ctx.alpha);
		read_reg(ptr, ctx.test);
		read_reg(ptr, ctx.fba);
		read_reg(ptr, ctx.frame);
		read_reg(ptr, ctx.zbuf);
	}

	read_reg(ptr, regs.rgbaq);
	read_reg(ptr, regs.st);
	read_reg(ptr, regs.uv.words[0]);
	read_reg(ptr, regs.fog.words[0]);
	// XYZ register, fill with dummy.
	ptr += sizeof(uint64_t);

	// Dummy GIFReg
	ptr += 2 * sizeof(uint32_t);

	// Dummy transfer X/Y
	ptr += 2 * sizeof(uint32_t);

	if (version >= 9)
	{
		// Dummy transfer params. Just skip those until we know what to do about them.
		ptr += GetSaveStateSize(version) - GetSaveStateSize(expected_version);
	}

	void *vram = iface.map_vram_write(0, GSLocalMemory::m_vmsize);
	read_data(ptr, vram, GSLocalMemory::m_vmsize);
	iface.end_vram_write(0, GSLocalMemory::m_vmsize);

	// 4 GIF paths
	for (int i = 0; i < 4; i++)
	{
		auto gif_path = iface.get_gif_path(i);
		gif_path.tag.NLOOP -= gif_path.loop;
		read_data(ptr, &gif_path.tag, sizeof(gif_path.tag));
		gif_path.loop = 0;
		read_reg(ptr, gif_path.reg);
	}

	// internal_Q
	read_reg(ptr, iface.get_register_state().internal_q);

	iface.clobber_register_state();
	return 0;
}

enum PGSTVEmulation
{
	PGS_TV_EMULATION_NONE = 0,
	PGS_TV_EMULATION_AUTO,
	PGS_TV_EMULATION_NTSC,
	PGS_TV_EMULATION_PAL,
};

enum PGSCable
{
	PGS_CABLE_COMPONENT = 0,
	PGS_CABLE_SVIDEO,
	PGS_CABLE_COMPOSITE,
};

enum PGSCompositeDecode
{
	PGS_COMPOSITE_COMB = 0,
	PGS_COMPOSITE_COMB_NOTCH,
	PGS_COMPOSITE_NOTCH,
};

enum PGSCRTPhosphor
{
	PGS_CRT_NONE,
	PGS_CRT_AUTO,
	PGS_CRT_NTSC_BT601,
	PGS_CRT_PAL_BT601,
	PGS_CRT_NTSC_1953
};

enum PGSGamma
{
	PGS_GAMMA_24 = 0,
	PGS_GAMMA_22,
	PGS_GAMMA_28,
};

void GSRendererPGS::VSync(u32 field, bool registers_written, bool refresh_frame)
{
	auto &wsi = device.get_wsi();
	bool frame_is_duped = false;

	if (!refresh_frame)
	{
		if (dump)
		{
			if (dump->VSync(field, dump_frames == 0, reinterpret_cast<GSPrivRegSet *>(priv)))
				dump.reset();
			else if (dump_frames != 0)
				dump_frames--;
		}

		iface.flush();
		iface.get_priv_register_state() = *priv;

		VSyncInfo info = {};

		info.phase = field;

		// Apparently this is needed for some games. It's set by game-fixes.
		// I assume this problem exists at a higher level than whatever GS controls, so we'll just
		// apply this hack too.
		if (GSConfig.InterlaceMode != GSInterlaceMode::Automatic)
			info.phase ^= (static_cast<int>(GSConfig.InterlaceMode) - 2) & 1;

		info.anti_blur = !GSConfig.PGSDisableCRTCEnhancements && GSConfig.PCRTCAntiBlur;

		info.force_progressive = GSConfig.PGSHighResScanout || !GSConfig.PGSDisableAutoProgressive;

		// The CRT simulation path assumes some kind of overscan.
		info.overscan = GSConfig.PGSTVEmulation != PGS_TV_EMULATION_NONE ||
						GSConfig.PGSPhosphorPrimaries != PGS_CRT_NONE ||
						GSConfig.PCRTCOverscan;

		info.crtc_offsets = GSConfig.PGSDisableCRTCEnhancements || GSConfig.PCRTCOffsets;

		info.dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
		info.dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		info.dst_layout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;

		// The scaling blur is technically a blur ...
		// Force it for analog emulation since we do the scaling there and double scaling is just bad.
		info.adapt_to_internal_horizontal_resolution = GSConfig.PGSTVEmulation != PGS_TV_EMULATION_NONE || GSConfig.PCRTCAntiBlur;

		// We want consistent results for any TV emulation.
		info.raw_circuit_scanout = GSConfig.PGSTVEmulation == PGS_TV_EMULATION_NONE && GSConfig.PGSPhosphorPrimaries == PGS_CRT_NONE;

		// High-res scanout only makes sense if we're doing FSR style upscale.
		info.high_resolution_scanout = GSConfig.PGSTVEmulation == PGS_TV_EMULATION_NONE &&
								   GSConfig.PGSPhosphorPrimaries == PGS_CRT_NONE &&
								   GSConfig.PGSHighResScanout != 0;

		// If we have CRT emulation, we deal with deinterlacing there.
		info.skip_deinterlace = GSConfig.PGSPhosphorPrimaries != PGS_CRT_NONE;
		last_frame_stats = iface.consume_flush_stats();

		// Do not allow frame skip if frame-dupe is used.
		frame_is_duped = !registers_written && last_frame_stats.num_render_passes == 0 && last_frame_stats.num_copies == 0 && iface.vsync_can_skip(info);

		// Don't waste GPU time scanning out the same thing twice.
		if (!frame_is_duped || !vsync.image || !m_snapshot.empty())
			vsync = iface.vsync(info);

		if (frame_is_duped && !GSConfig.SkipDuplicateFrames)
			wsi.set_next_present_is_duplicated();
	}

	Host::BeginPresentFrame();

	if (GSConfig.SkipDuplicateFrames && device.has_presented_in_current_swapchain && frame_is_duped && !refresh_frame)
	{
		PerformanceMetrics::Update(false, false, true);
		ImGuiManager::SkipFrame();
		return;
	}

	if (!refresh_frame)
	{
		PerformanceMetrics::OnGPUPresent(GetAndResetAccumulatedGPUTime(), 0, 0);
		PerformanceMetrics::Update(registers_written, last_frame_stats.num_render_passes != 0, false);
	}

	if (vsync.image)
	{
		last_internal_width = vsync.internal_width;
		last_internal_height = vsync.internal_height;
		if (vsync.high_resolution_scanout)
		{
			last_internal_width *= 2;
			last_internal_height *= 2;
		}
	}

	CRTFilter::FilterOptions crt_filter_options = {};

	// Setup backbuffer format outside begin_frame() / end_frame() to
	// get an instant response.
	if (GSConfig.PGSPhosphorPrimaries != PGS_CRT_NONE)
	{
		switch (GSConfig.PGSDisplayCalibration)
		{
			case PGS_DISPLAY_PQ_400:
			case PGS_DISPLAY_PQ_600:
			case PGS_DISPLAY_PQ_1000:
			case PGS_DISPLAY_PQ_FULL:
				wsi.set_backbuffer_format(BackbufferFormat::HDR10);
				crt_filter_options.hdr10 = true;
				crt_filter_options.hdr10_target_max_cll =
					display_calibration_to_max_cll(PGSDisplayCalibration(GSConfig.PGSDisplayCalibration));
				crt_filter_options.hdr10_target_paper_white = float(GSConfig.PGSPaperWhite);
				break;

			default:
				wsi.set_backbuffer_format(BackbufferFormat::UNORM);
				crt_filter_options.hdr10 = false;
				break;
		}
	}
	else if (GSConfig.PGSTVEmulation != PGS_TV_EMULATION_NONE &&
			 GSConfig.LinearPresent != GSPostBilinearMode::BilinearSharp)
	{
		// Straight passthrough of the UNORM image since we won't go through FSR.
		wsi.set_backbuffer_format(BackbufferFormat::UNORM);
	}
	else
	{
		// RCAS works in sRGB domain.
		wsi.set_backbuffer_format(BackbufferFormat::sRGB);
	}

	// High-refresh rate path.
	uint32_t frame_multiplier = 1;
	RefreshRateInfo refresh_info;

	if (wsi.get_refresh_rate_info(refresh_info) && refresh_info.refresh_duration)
	{
		// Are these values 100% accurate?
		float fps = priv->smode1.LC == SMODE1Bits::LC_ANALOG && priv->smode1.CMOD == SMODE1Bits::CMOD_PAL ? 50.0f : 59.94f;
		uint64_t target_period_ns = 1000000000ull / fps;
		bool force_vrr = false;

		if (refresh_info.mode != RefreshMode::VRR)
		{
			uint64_t interval = refresh_info.refresh_duration;

			// Try to align with monitor refresh rate if we're close enough.
			// If we cannot snap to a specific cycle we have to assume free-flowing relative timing.
			uint64_t alignment = target_period_ns % interval;
			if (alignment + interval / 256 >= interval || alignment <= interval / 256)
			{
				frame_multiplier = (target_period_ns + interval / 2) / interval;

				if (GSConfig.PGSHighRefreshInsertion && GSConfig.PGSPhosphorPrimaries != PGS_CRT_NONE)
				{
					// We'll spam N frames back to back.
					target_period_ns = interval;
				}
				else
				{
					// Let the compositor deal with correct pacing.
					target_period_ns = interval * frame_multiplier;
					frame_multiplier = 1;
				}
			}
			else if (GSConfig.PGSHighRefreshInsertion && GSConfig.PGSPhosphorPrimaries != PGS_CRT_NONE &&
					 target_period_ns >= refresh_info.refresh_duration)
			{
				// Try to approximate by inserting as many frames as we can at least, and accept some potential jank.
				frame_multiplier = target_period_ns / refresh_info.refresh_duration;
				// Every individual frame should be paced at a fraction of intended refresh.
				target_period_ns = interval;
				force_vrr = true;
			}
			else
			{
				// Try to pace the frames as intended. Don't do any weird rounding.
				force_vrr = true;
			}
		}
		else if (GSConfig.PGSHighRefreshInsertion)
		{
			// If we know we have VRR we can go to town with high refresh rate framegen.
			// Duplicate as many frames as we can within the refresh rate.
			frame_multiplier = target_period_ns / refresh_info.refresh_duration;

			// Every individual frame should be paced at a fraction of intended refresh.
			// Since we have VRR, this should be smoothly paced.
			target_period_ns = target_period_ns / frame_multiplier;
		}

		wsi.set_target_presentation_time(0, target_period_ns, force_vrr);
	}

	// We cannot reliably get timing feedback when running unlocked since we will exhaust queues.
	if (wsi.get_present_mode() == PresentMode::SyncToVBlank)
	{
		wsi.set_enable_timing_feedback(true);
	}
	else
	{
		wsi.set_enable_timing_feedback(false);
		wsi.set_target_presentation_time(0, 0, false);
	}

	// High refresh setup requires a lot of swapchain images ...
	if (frame_multiplier > 1)
		wsi.set_frame_duplication_aware(true, frame_multiplier * 3);

	bool analog_output_is_valid = false;

	for (uint32_t frame_index = 0; frame_index < frame_multiplier; frame_index++)
	{
		if (!device.has_wsi_begin_frame)
			device.has_wsi_begin_frame = wsi.begin_frame();

		if (!device.has_wsi_begin_frame)
			return;

		uint32_t output_width = wsi.get_device().get_swapchain_view().get_view_width();
		uint32_t output_height = wsi.get_device().get_swapchain_view().get_view_height();

		if (frame_index != 0)
			wsi.set_next_present_is_duplicated();

		auto &dev = wsi.get_device();

		float vp_offset_x = 0.0f;
		float vp_offset_y = 0.0f;
		float vp_width = 0.0f;
		float vp_height = 0.0f;

		if (GSConfig.PGSPhosphorPrimaries != PGS_CRT_NONE || GSConfig.LinearPresent != GSPostBilinearMode::BilinearSharp || !vsync.image)
			fsr_render_target.reset();

		bool fsr_render_is_valid = frame_is_duped && bool(fsr_render_target);

		// Only run analog emulation if game is emitting an analog signal.
		// Some games can emit VGA which obviously skips all this stuff ...
		if (frame_index == 0 && GSConfig.PGSTVEmulation != PGS_TV_EMULATION_NONE && priv->smode1.LC == SMODE1Bits::LC_ANALOG)
		{
			AnalogVideoFilter::Options analog_options = {};

			switch (GSConfig.PGSCable)
			{
				case PGS_CABLE_COMPOSITE: analog_options.cable = AnalogVideoFilter::Cable::Composite; break;
				case PGS_CABLE_SVIDEO: analog_options.cable = AnalogVideoFilter::Cable::SVideo; break;
				case PGS_CABLE_COMPONENT: analog_options.cable = AnalogVideoFilter::Cable::Component; break;
				default: break;
			}

			switch (GSConfig.PGSTVEmulation)
			{
				case PGS_TV_EMULATION_NTSC:
					analog_options.system = AnalogVideoFilter::System::NTSC;
					break;

				case PGS_TV_EMULATION_PAL:
					analog_options.system = AnalogVideoFilter::System::PAL;
					break;

				case PGS_TV_EMULATION_AUTO:
					analog_options.system = priv->smode1.CMOD == SMODE1Bits::CMOD_NTSC ?
							AnalogVideoFilter::System::NTSC :
					AnalogVideoFilter::System::PAL;
					break;

				default:
					break;
			}

			if (analog_filter.get_options().cable != analog_options.cable ||
				analog_filter.get_options().system != analog_options.system)
			{
				auto line_counter = analog_filter.get_line_counter();
				analog_filter = {};

				analog_filter.reset_line_counter(line_counter);
				analog_filter.init(wsi.get_device(), analog_options);
			}
		}

		if (vsync.image)
		{
			uint32_t new_width = output_width;
			uint32_t new_height = output_height;

			float display_aspect = float(output_width) / float(output_height);
			float game_aspect = GetCurrentAspectRatioFloat(priv->smode1.CMOD == 0);

			float horizontal_scanout_ratio = float(vsync.internal_width) / float(vsync.mode_width);
			float vertical_scanout_ratio = float(vsync.internal_height) / float(vsync.mode_height);
			game_aspect *= horizontal_scanout_ratio / vertical_scanout_ratio;
			last_display_aspect_ratio = game_aspect;

			if (display_aspect > game_aspect)
				new_width = uint32_t(std::round(float(output_height) * game_aspect));
			else
				new_height = uint32_t(std::round(float(output_width) / game_aspect));

			if (GSConfig.IntegerScaling)
			{
				const float scale = (vsync.image->get_width() >= vsync.image->get_height()) ?
				                        static_cast<float>(new_width) / vsync.image->get_width() :
				                        static_cast<float>(new_height) / vsync.image->get_height();
				if (scale > 1.0f)
				{
					const float adjust = std::floor(scale) / scale;
					new_width = std::max(1u, static_cast<uint32_t>(std::round(new_width * adjust)));
					new_height = std::max(1u, static_cast<uint32_t>(std::round(new_height * adjust)));
				}
			}

			vp_offset_x = std::round(0.5f * float(output_width - new_width));
			output_width = new_width;
			vp_offset_y = std::round(0.5f * float(output_height - new_height));
			output_height = new_height;

			// Safeguard against ridiculous situations.
			if (!output_width)
				output_width = 1;
			if (!output_height)
				output_height = 1;

			vp_width = float(output_width);
			vp_height = float(output_height);
		}

		auto cmd = dev.request_command_buffer();
		QueryPoolHandle gpu_start_timestamp;
		if (gpu_timing_enabled)
			gpu_start_timestamp = cmd->write_timestamp(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

		render_ui_prepare(*cmd);

		if (frame_index == 0 && vsync.image && GSConfig.PGSTVEmulation != PGS_TV_EMULATION_NONE && priv->smode1.LC == SMODE1Bits::LC_ANALOG)
		{
			AnalogVideoFilter::FilterOptions filter_options = {};
			filter_options.dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			filter_options.dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
			filter_options.dst_layout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
			filter_options.phase = vsync.interlaced ? vsync.interlace_phase : 0;

			switch (GSConfig.PGSCompositeDecode)
			{
				case PGS_COMPOSITE_COMB:
					filter_options.line_comb = true;
					filter_options.skip_notch = true;
					break;

				case PGS_COMPOSITE_COMB_NOTCH:
					filter_options.line_comb = true;
					filter_options.skip_notch = false;
					break;

				default:
					filter_options.line_comb = false;
					filter_options.skip_notch = false;
					break;
			}

			filter_options.input_sampling_rate_mhz = vsync.sampling_rate_mhz * (vsync.interlaced ? 1.0f : 0.5f);
			analog_filter.run_filter(*cmd, vsync.image->get_view(), filter_options);
			analog_output_is_valid = true;
		}

		if (vsync.image && GSConfig.LinearPresent == GSPostBilinearMode::BilinearSharp)
		{
			if (!fsr_render_target || fsr_render_target->get_width() != output_width || fsr_render_target->get_height() != output_height)
			{
				auto fsr_info = ImageCreateInfo::render_target(output_width, output_height, VK_FORMAT_R8G8B8A8_UNORM);
				fsr_info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
				fsr_info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
				fsr_info.misc |= IMAGE_MISC_MUTABLE_SRGB_BIT;
				fsr_render_target = dev.create_image(fsr_info);
				fsr_render_is_valid = false;
			}
		}

		// No need to do the upscaling twice when duping frames.
		if (vsync.image && GSConfig.PGSPhosphorPrimaries != PGS_CRT_NONE)
		{
			crt_filter_options.phase = vsync.interlaced ? vsync.interlace_phase : 0;
			crt_filter_options.progressive = !vsync.interlaced;
			crt_filter_options.double_strike = vsync.double_strike;
			crt_filter_options.feedback = 0.02f;
			crt_filter_options.feedback = std::pow(crt_filter_options.feedback, 1.0f / frame_multiplier);

			crt_filter_options.input_strength = float(GSConfig.PGSExposure) / 100.0f;

			// High refresh rate decay.
			if (frame_index != 0)
				crt_filter_options.input_strength = 0.0f;

			if (analog_output_is_valid)
			{
				// Compensate for BT.601 720-pix container and overscan. 640 / 720 = ~0.89
				crt_filter_options.input_rect = { 0.055f, 0.04f, 0.89f, 0.92f };
			}
			else
			{
				// Overscan mode width is about 712 pixels, 640 / 712 ~= 0.9
				crt_filter_options.input_rect = { 0.05f, 0.04f, 0.9f, 0.92f };
			}

			crt_filter_options.scan_factor_narrow = 2.0f + 8.0f * float(GSConfig.PGSScanlineSharpness) / 100.0f;
			float breathe = float(GSConfig.PGSScanlineBreathing) / 100.0f;
			crt_filter_options.scan_factor_wide = crt_filter_options.scan_factor_narrow * (1.0f - 0.5f * breathe);

			switch (GSConfig.PGSPhosphorPrimaries)
			{
				case PGS_CRT_AUTO:
					crt_filter_options.phosphor_primaries = priv->smode1.CMOD == SMODE1Bits::CMOD_NTSC ?
						CRTFilter::Primaries::BT601_525 : CRTFilter::Primaries::BT601_625;
					break;

				case PGS_CRT_NTSC_BT601:
					crt_filter_options.phosphor_primaries = CRTFilter::Primaries::BT601_525;
					break;

				case PGS_CRT_PAL_BT601:
					crt_filter_options.phosphor_primaries = CRTFilter::Primaries::BT601_625;
					break;

				case PGS_CRT_NTSC_1953:
					crt_filter_options.phosphor_primaries = CRTFilter::Primaries::NTSC_Legacy;
					break;

				default:
					break;
			}

			switch (GSConfig.PGSPhosphorGamma)
			{
				case PGS_GAMMA_24: crt_filter_options.gamma = 2.4f; break;
				case PGS_GAMMA_22: crt_filter_options.gamma = 2.2f; break;
				case PGS_GAMMA_28: crt_filter_options.gamma = 2.8f; break;
				default: break;
			}

			crt_filter_options.aperture_grille = GSConfig.PGSApertureGrille != 0;
			crt_filter_options.bloom_strength = 1.2f * float(GSConfig.PGSPhosphorBloom) / 100.0f;

			// For wide-screen, assume a more HD CRT TV style with far higher TVL.
			float game_aspect = GetCurrentAspectRatioFloat(priv->smode1.CMOD == 0);
			crt_filter_options.tvl = uint32_t(480.0f * game_aspect);

			crt_filter.run_filter_prepass(*cmd,
				analog_output_is_valid ? analog_filter.get_output() : vsync.image->get_view(),
				crt_filter_options, output_width, output_height);
		}
		else if (vsync.image && fsr_render_target && !fsr_render_is_valid)
		{
			render_fsr(*cmd, analog_output_is_valid ? analog_filter.get_output() : vsync.image->get_view());
		}

		BufferHandle screenshot_readback;
		u32 screenshot_width = 0;
		u32 screenshot_height = 0;
		bool screenshot_bgra = false;
		bool screenshot_failed = false;
		const bool capture_screenshot = frame_index == 0 && !m_snapshot.empty() && vsync.image;
		if (capture_screenshot)
		{
			screenshot_width = static_cast<u32>(std::max(1.0f, std::round(vp_width)));
			screenshot_height = static_cast<u32>(std::max(1.0f, std::round(vp_height)));
			const VkFormat screenshot_format =
				wsi.get_backbuffer_format() == BackbufferFormat::sRGB ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
			auto screenshot_info = ImageCreateInfo::render_target(
				screenshot_width, screenshot_height, screenshot_format);
			screenshot_info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
			auto screenshot_target = dev.create_image(screenshot_info);

			if (screenshot_target)
			{
				RenderPassInfo rp = {};
				rp.num_color_attachments = 1;
				rp.color_attachments[0] = &screenshot_target->get_view();
				rp.clear_attachments = 1u << 0;
				rp.store_attachments = 1u << 0;
				rp.clear_color[0].float32[0] = 0.0f;
				rp.clear_color[0].float32[1] = 0.0f;
				rp.clear_color[0].float32[2] = 0.0f;
				rp.clear_color[0].float32[3] = 1.0f;

				cmd->begin_render_pass(rp);
				if (GSConfig.PGSPhosphorPrimaries != PGS_CRT_NONE)
				{
					cmd->set_viewport({0, 0,
						static_cast<float>(screenshot_width),
						static_cast<float>(screenshot_height), 0, 1});
					crt_filter.run_filter_encode(*cmd, crt_filter_options);
				}
				else if (fsr_render_target)
				{
					render_rcas(*cmd, fsr_render_target->get_view(), 0, 0,
						static_cast<float>(screenshot_width),
						static_cast<float>(screenshot_height));
				}
				else
				{
					render_blit(*cmd, analog_output_is_valid ? analog_filter.get_output() : vsync.image->get_view(), 0, 0,
						static_cast<float>(screenshot_width),
						static_cast<float>(screenshot_height));
				}
				cmd->end_render_pass();

				screenshot_failed = !QueueImageReadback(*cmd, *screenshot_target,
					&screenshot_readback, screenshot_width, screenshot_height, &screenshot_bgra);
			}
			else
			{
				screenshot_failed = true;
			}
		}

		cmd->begin_render_pass(dev.get_swapchain_render_pass(SwapchainRenderPass::ColorOnly));
		if (vsync.image)
		{
			// The RCAS pass is basically free.
			if (GSConfig.PGSPhosphorPrimaries != PGS_CRT_NONE)
			{
				cmd->set_viewport({ vp_offset_x, vp_offset_y, vp_width, vp_height, 0, 1 });
				crt_filter.run_filter_encode(*cmd, crt_filter_options);
			}
			else if (fsr_render_target)
			{
				render_rcas(*cmd, fsr_render_target->get_view(), vp_offset_x, vp_offset_y, vp_width, vp_height);
			}
			else
			{
				render_blit(*cmd, analog_output_is_valid ? analog_filter.get_output() : vsync.image->get_view(),
					vp_offset_x, vp_offset_y, vp_width, vp_height);
			}
		}
		render_ui_flush(*cmd);
		cmd->end_render_pass();

		if (gpu_timing_enabled)
		{
			auto gpu_end_timestamp = cmd->write_timestamp(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
			gpu_timestamps.push_back({ std::move(gpu_start_timestamp), std::move(gpu_end_timestamp) });
		}
		dev.submit(cmd);
		if (screenshot_readback || screenshot_failed)
		{
			if (screenshot_readback)
			{
				dev.wait_idle();
				screenshot_failed = !SaveScreenshotReadback(*screenshot_readback,
					screenshot_width, screenshot_height, screenshot_bgra);
			}

			if (screenshot_failed)
				Host::AddIconOSDMessage("GSScreenshot", ICON_FA_CAMERA,
					TRANSLATE_SV("GS", "Failed to render/download screenshot."), Host::OSD_ERROR_DURATION);

			m_snapshot.clear();
		}

		wsi.end_frame();
		device.has_wsi_begin_frame = false;
		render_ui_end();
	}

	// For pacing purposes.
	device.has_wsi_begin_frame = wsi.begin_frame();
	device.has_presented_in_current_swapchain = true;
}

void GSRendererPGS::Transfer(const u8* mem, u32 size)
{
	size *= 16;
	iface.gif_transfer(3, mem, size);
	if (dump)
		dump->Transfer(3, mem, size);
}

void GSRendererPGS::ReadFIFO(u8 *mem, u32 size)
{
	iface.read_transfer_fifo(mem, size);
	if (dump)
		dump->ReadFIFO(size);
}

void GSRendererPGS::GetInternalResolution(int *width, int *height)
{
	*width = int(last_internal_width);
	*height = int(last_internal_height);
}

bool GSDevicePGS::UpdateWindow()
{
	std::optional<WindowInfo> window = Host::AcquireRenderWindow(true);
	if (window.has_value())
	{
		window_info = window.value();
		wsi.deinit_surface_and_swapchain();
		return wsi.init_surface_swapchain();
	}
	else
		return false;
}

bool GSRendererPGS::UpdateWindow()
{
	iface.flush();
	return device.UpdateWindow();
}

void GSDevicePGS::ResizeWindow(int width, int height, float /*scale*/)
{
	resize = true;
	window_info.surface_width = width;
	window_info.surface_height = height;
	// TODO: No idea what to do about scale.
}

const WindowInfo &GSDevicePGS::GetWindowInfo() const
{
	return window_info;
}

void GSDevicePGS::SetVSyncMode(GSVSyncMode mode, bool /*allow_present_throttle*/)
{
	if (mode == GSVSyncMode::FIFO)
		wsi.set_present_mode(PresentMode::SyncToVBlank);
	else if (mode == GSVSyncMode::Mailbox)
		wsi.set_present_mode(PresentMode::UnlockedNoTearing);
	else
		wsi.set_present_mode(PresentMode::UnlockedMaybeTear);
	// Unknown what allow_present_throttle means.
}

VkSurfaceKHR GSDevicePGS::create_surface(VkInstance instance, VkPhysicalDevice gpu)
{
	if (window_info.type == WindowInfo::Type::Surfaceless)
	{
		std::optional<WindowInfo> window = Host::AcquireRenderWindow(true);
		if (window.has_value())
			window_info = window.value();
	}

	if (window_info.type == WindowInfo::Type::Surfaceless)
		return VK_NULL_HANDLE;

#if defined(X11_API)
	if (window_info.type == WindowInfo::Type::X11)
	{
		VkXlibSurfaceCreateInfoKHR info = { VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR };
		info.dpy = static_cast<Display *>(window_info.display_connection);
		info.window = reinterpret_cast<Window>(window_info.window_handle);
		VkSurfaceKHR surface;
		if (vkCreateXlibSurfaceKHR(instance, &info, nullptr, &surface) == VK_SUCCESS)
			return surface;
	}
#endif
#if defined(WAYLAND_API)
	if (window_info.type == WindowInfo::Type::Wayland)
	{
		VkWaylandSurfaceCreateInfoKHR info = { VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR };
		info.display = static_cast<wl_display *>(window_info.display_connection);
		info.surface = static_cast<wl_surface *>(window_info.window_handle);
		VkSurfaceKHR surface;
		if (vkCreateWaylandSurfaceKHR(instance, &info, nullptr, &surface) == VK_SUCCESS)
			return surface;
	}
#endif
#if defined(_WIN32)
	if (window_info.type == WindowInfo::Type::Win32)
	{
		VkWin32SurfaceCreateInfoKHR info = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
		info.hwnd = static_cast<HWND>(window_info.window_handle);
		VkSurfaceKHR surface;
		if (vkCreateWin32SurfaceKHR(instance, &info, nullptr, &surface) == VK_SUCCESS)
			return surface;
	}
#endif

	return VK_NULL_HANDLE;
}

void GSDevicePGS::destroy_surface(VkInstance instance, VkSurfaceKHR surface)
{
	WSIPlatform::destroy_surface(instance, surface);
}

std::vector<const char *> GSDevicePGS::get_instance_extensions()
{
	return {
		VK_KHR_SURFACE_EXTENSION_NAME,
#if defined(X11_API)
		VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
#endif
#if defined(WAYLAND_API)
		VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
#endif
#if defined(_WIN32)
		VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#endif
	};
}

std::vector<const char *> GSDevicePGS::get_device_extensions()
{
	return { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
}

bool GSDevicePGS::alive(WSI &)
{
	return true;
}

uint32_t GSDevicePGS::get_surface_width()
{
	return window_info.surface_width;
}

uint32_t GSDevicePGS::get_surface_height()
{
	return window_info.surface_height;
}

void GSDevicePGS::poll_input()
{
	// Dummy, we don't care about input here.
}

void GSDevicePGS::poll_input_async(Granite::InputTrackerHandler *)
{
	// Dummy, we don't care about input here.
}

void GSDevicePGS::event_swapchain_destroyed()
{
	WSIPlatform::event_swapchain_destroyed();
	has_wsi_begin_frame = false;
	has_presented_in_current_swapchain = false;
}

bool GSRendererPGS::QueueImageReadback(CommandBuffer &cmd, const Image &image,
	BufferHandle *readback, u32 width, u32 height, bool *bgra)
{
	const VkFormat format = image.get_format();
	const bool rgba = (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB);
	*bgra = (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB);
	if (!rgba && !*bgra)
		return false;

	if (width == 0 || height == 0)
		return false;

	BufferCreateInfo buffer_info = {};
	buffer_info.domain = BufferDomain::CachedHost;
	buffer_info.size = static_cast<VkDeviceSize>(width) * height * sizeof(u32);
	buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	*readback = device.get_wsi().get_device().create_buffer(buffer_info);
	if (!*readback)
		return false;

	cmd.image_barrier(image, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

	VkImageSubresourceLayers subresource = {};
	subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	subresource.mipLevel = 0;
	subresource.baseArrayLayer = 0;
	subresource.layerCount = 1;
	cmd.copy_image_to_buffer(**readback, image, 0, {0, 0, 0}, {width, height, 1}, 0, 0, subresource);
	cmd.buffer_barrier(**readback,
		VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
	return true;
}

bool GSRendererPGS::SaveScreenshotReadback(const Buffer &readback, u32 width, u32 height, bool bgra)
{
	auto &dev = device.get_wsi().get_device();

	const u8 *mapped = static_cast<const u8 *>(dev.map_host_buffer(readback, MEMORY_ACCESS_READ_BIT));
	if (!mapped)
		return false;

	std::vector<u32> pixels;
	pixels.resize(static_cast<size_t>(width) * height);
	if (!bgra)
	{
		for (size_t i = 0; i < pixels.size(); i++)
		{
			const u32 pixel = reinterpret_cast<const u32*>(mapped)[i];
			pixels[i] = pixel | 0xff000000u;
		}
	}
	else
	{
		for (size_t i = 0; i < pixels.size(); i++)
		{
			const u8 b = mapped[i * 4 + 0];
			const u8 g = mapped[i * 4 + 1];
			const u8 r = mapped[i * 4 + 2];
			pixels[i] = static_cast<u32>(r) | (static_cast<u32>(g) << 8) |
				(static_cast<u32>(b) << 16) | 0xff000000u;
		}
	}

	dev.unmap_host_buffer(readback, MEMORY_ACCESS_READ_BIT);
	GSCompressAndWriteScreenshot(GSGetScreenshotFilename(m_snapshot), width, height, std::move(pixels));
	return true;
}

void GSRendererPGS::QueueGSDump(const std::string &path, u32 gsdump_frames)
{
	freezeData fd = {0, nullptr};
	Freeze(&fd, true);
	fd.data = new u8[fd.size];
	Freeze(&fd, false);

	if (GSConfig.GSDumpCompression == GSDumpCompressionMethod::Uncompressed)
	{
		dump = GSDumpBase::CreateUncompressedDump(path, VMManager::GetDiscSerial(),
		                                          VMManager::GetDiscCRC(), 0, 0,
		                                          nullptr, fd, reinterpret_cast<GSPrivRegSet *>(priv));
	}
	else if (GSConfig.GSDumpCompression == GSDumpCompressionMethod::LZMA)
	{
		dump = GSDumpBase::CreateXzDump(path, VMManager::GetDiscSerial(),
		                                VMManager::GetDiscCRC(), 0, 0,
		                                nullptr, fd, reinterpret_cast<GSPrivRegSet *>(priv));
	}
	else
	{
		dump = GSDumpBase::CreateZstDump(path, VMManager::GetDiscSerial(),
		                                 VMManager::GetDiscCRC(), 0, 0,
		                                 nullptr, fd, reinterpret_cast<GSPrivRegSet *>(priv));
	}

	dump_frames = gsdump_frames;
	delete[] fd.data;
}

void GSRendererPGS::QueueSnapshot(const std::string &path, u32 gsdump_frames)
{
	if (gsdump_frames > 0)
	{
		QueueGSDump(path.empty() ? GSGetBaseSnapshotFilename() : path, gsdump_frames);
		return;
	}

	if (!m_snapshot.empty())
		return;

	// Match the common renderer's historical explicit-path behavior.
	if (path.size() > 4 && StringUtil::EndsWithNoCase(path, ".png"))
		m_snapshot = path.substr(0, path.size() - 4);
	else
		m_snapshot = GSGetBaseSnapshotFilename();
}

void GSRendererPGS::StopGSDump()
{
	dump.reset();
	dump_frames = 0;
}

const VkApplicationInfo *GSDevicePGS::get_application_info()
{
	static const VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr,
	                                       "pcsx2", 0, "Granite", 0, VK_API_VERSION_1_3 };
	return &app;
}

GSDevicePGS::~GSDevicePGS()
{
	DestroyImGuiTextures();
}

void GSDevicePGS::DestroyImGuiTextures()
{
	if (!ImGui::GetCurrentContext())
		return;

	for (auto *im_tex : ImGui::GetPlatformIO().Textures)
	{
		if (im_tex->Status != ImTextureStatus_Destroyed)
		{
			auto *img = static_cast<Image *>(im_tex->BackendUserData);
			if (img == nullptr)
				continue;

			pxAssert(im_tex->RefCount == 1);

			img->release_reference();
			im_tex->SetTexID(ImTextureID_Invalid);
			im_tex->BackendUserData = nullptr;
			im_tex->Status = ImTextureStatus_Destroyed;
		}
	}
}

void GSRendererPGS::render_ui_prepare(CommandBuffer &cmd)
{
	if (GSDumpReplayer::IsReplayingDump())
		GSDumpReplayer::RenderUI();

	FullscreenUI::Render();
	ImGuiManager::RenderOSD();
	ImGui::Render();

	// Loose copy paste from GSDevice::UpdateImGuiTextures().
	for (auto *im_tex : ImGui::GetPlatformIO().Textures)
	{
		switch (im_tex->Status)
		{
			case ImTextureStatus_OK:
			case ImTextureStatus_Destroyed:
				continue;

			case ImTextureStatus_WantCreate:
			{
				ImageCreateInfo info = ImageCreateInfo::immutable_2d_image(im_tex->Width, im_tex->Height, VK_FORMAT_R8G8B8A8_UNORM);
				info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
				info.initial_layout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
				ImageInitialData init = { im_tex->GetPixels() };
				auto img = device.get_device().create_image(info, &init);
				im_tex->SetTexID(reinterpret_cast<ImTextureID>(img.get()));
				im_tex->BackendUserData = img.release();
				im_tex->Status = ImTextureStatus_OK;
				break;
			}

			case ImTextureStatus_WantUpdates:
			{
				const int upload_x = im_tex->UpdateRect.x;
				const int upload_y = im_tex->UpdateRect.y;
				const int upload_w = im_tex->UpdateRect.w;
				const int upload_h = im_tex->UpdateRect.h;
				const int upload_pitch = upload_w * im_tex->BytesPerPixel;

				auto *img = static_cast<Image *>(im_tex->BackendUserData);

				cmd.image_barrier(*img, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
					VK_ACCESS_TRANSFER_WRITE_BIT);

				auto *ptr = static_cast<uint8_t *>(cmd.update_image(*img, { upload_x, upload_y }, { uint32_t(upload_w), uint32_t(upload_h), 1 },
					upload_w, upload_h, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }));

				for (int y = 0; y < upload_h; y++)
					memcpy(ptr + upload_pitch * y, im_tex->GetPixelsAt(upload_x, upload_y + y), upload_pitch);

				cmd.image_barrier(*img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
					VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
					VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

				im_tex->Status = ImTextureStatus_OK;
				break;
			}

			case ImTextureStatus_WantDestroy:
			{
				auto *gs_tex = static_cast<Image *>(im_tex->BackendUserData);
				if (gs_tex == nullptr)
					break;

				gs_tex->release_reference();
				im_tex->SetTexID(ImTextureID_Invalid);
				im_tex->BackendUserData = nullptr;
				im_tex->Status = ImTextureStatus_Destroyed;
				break;
			}

			default:
				pxAssert(false);
				break;
		}
	}
}

void GSRendererPGS::render_ui_flush(CommandBuffer &cmd)
{
	// Loosely based on GSDeviceVK::RenderImGui().
	const ImDrawData *draw_data = ImGui::GetDrawData();

	auto width = cmd.get_device().get_swapchain_view().get_view_width();
	auto height = cmd.get_device().get_swapchain_view().get_view_height();
	cmd.set_viewport({ 0, 0, float(width), float(height), 0, 1 });

	for (int n = 0; n < draw_data->CmdLists.Size; n++)
	{
		const ImDrawList *cmd_list = draw_data->CmdLists[n];
		memcpy(cmd.allocate_vertex_data(0, cmd_list->VtxBuffer.Size * sizeof(ImDrawVert), sizeof(ImDrawVert)),
			cmd_list->VtxBuffer.Data, cmd_list->VtxBuffer.Size * sizeof(ImDrawVert));
		memcpy(cmd.allocate_index_data(cmd_list->IdxBuffer.Size * sizeof(uint16_t), VK_INDEX_TYPE_UINT16),
			cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size * sizeof(uint16_t));
		cmd.set_vertex_attrib(0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ImDrawVert, pos));
		cmd.set_vertex_attrib(1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ImDrawVert, uv));
		cmd.set_vertex_attrib(2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(ImDrawVert, col));

		for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
		{
			const ImDrawCmd *pcmd = &cmd_list->CmdBuffer[cmd_i];
			pxAssert(!pcmd->UserCallback);

			auto scissor_x = int(std::max(0.0f, pcmd->ClipRect.x));
			auto scissor_y = int(std::max(0.0f, pcmd->ClipRect.y));
			auto scissor_z = int(pcmd->ClipRect.z);
			auto scissor_w = int(pcmd->ClipRect.w);

			if (scissor_z <= scissor_x || scissor_w <= scissor_y)
				continue;

			cmd.set_scissor({{ int(scissor_x), int(scissor_y) },
				{ uint32_t(scissor_z - scissor_x), uint32_t(scissor_w - scissor_y) }});

			// Since we don't have the GSTexture...
			auto *img = reinterpret_cast<Image *>(pcmd->GetTexID());
			if (img)
				cmd.set_texture(0, 0, img->get_view(), StockSampler::LinearClamp);

			vec2 inv_size = { 1.0f / float(device.window_info.surface_width), 1.0f / float(device.window_info.surface_height) };
			cmd.push_constants(&inv_size, 0, sizeof(inv_size));

			cmd.set_program(ui_program[img ? 1 : 0][device.get_wsi().get_backbuffer_format() == BackbufferFormat::sRGB]);
			cmd.set_transparent_sprite_state();
			cmd.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
			cmd.set_depth_test(false, false);
			cmd.set_cull_mode(VK_CULL_MODE_NONE);

			cmd.draw_indexed(pcmd->ElemCount, 1, pcmd->IdxOffset, pcmd->VtxOffset, 0);
		}
	}
}

void GSRendererPGS::render_ui_end()
{
	ImGuiManager::NewFrame();
}

GSTexture *GSDevicePGS::CreateTexture(u32 width, u32 height, const void* pixels, u32 pitch)
{
	auto info = ImageCreateInfo::immutable_2d_image(width, height, VK_FORMAT_R8G8B8A8_UNORM);
	ImageInitialData initial_data = { pixels, pitch / 4 };
	return new GSTexturePGS(wsi.get_device().create_image(info, &initial_data));
}
