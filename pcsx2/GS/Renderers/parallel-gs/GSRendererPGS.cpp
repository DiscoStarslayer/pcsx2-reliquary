// SPDX-FileCopyrightText: 2024 Hans-Kristian Arntzen
// SPDX-License-Identifier: LGPL-3.0+

#include "GSRendererPGS.h"
#include "GS/GSState.h"
#include "GS.h"
#include "math.hpp"
#include "muglm/muglm_impl.hpp"
#include "shaders/slangmosh.hpp"
#include "PerformanceMetrics.h"
#include "VMManager.h"
#include "command_buffer.hpp"

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
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
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
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
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
	cmd.set_sampler(0, 0, Vulkan::StockSampler::NearestClamp);
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
	                      Vulkan::StockSampler::LinearClamp : Vulkan::StockSampler::NearestClamp);
	cmd.set_opaque_state();
	cmd.set_depth_test(false, false);
	cmd.set_program(blit_program);

	cmd.set_viewport({ offset_x, offset_y, width, height, 0.0f, 1.0f });
	cmd.draw(3);
}

GSRendererPGS::GSRendererPGS(u8 *basemem)
	: priv(reinterpret_cast<PrivRegisterState *>(basemem))
{
	wsi.set_backbuffer_format(BackbufferFormat::sRGB);
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

bool GSRendererPGS::Init()
{
	// Always force the reload, since the other backends may clobber the volk pointers.
	if (!Context::init_loader(nullptr, true))
		return false;

	wsi.set_platform(this);
	wsi.set_frame_duplication_aware(true);
	bool ret = wsi.init_simple(1, {});
	if (!ret)
		return false;

	// We will cycle through many memory contexts per frame most likely.
	wsi.get_device().init_frame_contexts(12);

	ResourceLayout layout;
	Shaders<> suite(wsi.get_device(), layout, 0);
	upscale_program = wsi.get_device().request_program(suite.upscale_vert, suite.upscale_frag);
	sharpen_program = wsi.get_device().request_program(suite.sharpen_vert, suite.sharpen_frag);
	blit_program = wsi.get_device().request_program(suite.quad, suite.blit);

	GSOptions opts = {};
	opts.vram_size = GSLocalMemory::m_vmsize;

	auto parsed = parse_super_sampling_options(GSConfig.PGSSuperSampling);
	opts.super_sampling = parsed.super_sampling;
	opts.ordered_super_sampling = parsed.ordered;

	opts.dynamic_super_sampling = true;
	opts.super_sampled_textures = GSConfig.PGSSuperSampleTextures != 0;
	if (!iface.init(&wsi.get_device(), opts))
		return false;

	current_super_sampling = opts.super_sampling;
	current_ordered_super_sampling = opts.ordered_super_sampling;
	current_super_sample_textures = opts.super_sampled_textures;

	Hacks hacks;
	hacks.disable_mipmaps = GSConfig.PGSDisableMipmaps != 0;
	hacks.unsynced_readbacks = GSConfig.PGSDisableReadbackSync != 0;
	hacks.backbuffer_promotion = GSConfig.PGSSharpBackbuffer != 0;
	hacks.allow_blend_demote = GSConfig.PGSBlendDemotion != 0;
	iface.set_hacks(hacks);

	return true;
}

void GSRendererPGS::Reset(bool /*hardware_reset*/)
{
	iface.reset_context_state();
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

void GSRendererPGS::VSync(u32 field, bool registers_written)
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

	info.anti_blur = GSConfig.PCRTCAntiBlur;
	info.force_progressive = true;
	info.overscan = GSConfig.PCRTCOverscan;
	info.crtc_offsets = GSConfig.PCRTCOffsets;
	info.dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
	info.dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	info.dst_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	// The scaling blur is technically a blur ...
	info.adapt_to_internal_horizontal_resolution = GSConfig.PCRTCAntiBlur;
	info.raw_circuit_scanout = true;
	info.high_resolution_scanout = GSConfig.PGSHighResScanout != 0;

	auto stats = iface.consume_flush_stats();
	bool frame_is_duped = !registers_written && stats.num_render_passes == 0 && stats.num_copies == 0 && iface.vsync_can_skip(info);

	// Don't waste GPU time scanning out the same thing twice.
	if (!frame_is_duped || !vsync.image)
		vsync = iface.vsync(info);

	// Experimental heuristic. If running on an iGPU / APU, we're quite wary about draining the GPU for work
	// in GPU bound scenarios. This should keep the GPU better fed at the cost of a little more latency.
	if (wsi.get_device().get_gpu_properties().deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU &&
	    frame_is_duped && !GSConfig.SkipDuplicateFrames)
	{
		wsi.set_next_present_is_duplicated();
	}

	if (GSConfig.SkipDuplicateFrames && has_presented_in_current_swapchain && frame_is_duped)
	{
		PerformanceMetrics::Update(false, false, true);
		return;
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

	if (!has_wsi_begin_frame)
		has_wsi_begin_frame = wsi.begin_frame();

	if (!has_wsi_begin_frame)
		return;

	auto &dev = wsi.get_device();

	float vp_offset_x = 0.0f;
	float vp_offset_y = 0.0f;
	float vp_width = 0.0f;
	float vp_height = 0.0f;

	if (GSConfig.LinearPresent != GSPostBilinearMode::BilinearSharp || !vsync.image)
		fsr_render_target.reset();

	bool fsr_render_is_valid = frame_is_duped && bool(fsr_render_target);

	if (vsync.image)
	{
		uint32_t fsr_width = dev.get_swapchain_view().get_view_width();
		uint32_t fsr_height = dev.get_swapchain_view().get_view_height();
		uint32_t new_width = fsr_width;
		uint32_t new_height = fsr_height;

		float display_aspect = float(fsr_width) / float(fsr_height);
		float game_aspect = GetCurrentAspectRatioFloat(priv->smode1.CMOD == 0);

		float horizontal_scanout_ratio = float(vsync.internal_width) / float(vsync.mode_width);
		float vertical_scanout_ratio = float(vsync.internal_height) / float(vsync.mode_height);
		game_aspect *= horizontal_scanout_ratio / vertical_scanout_ratio;

		if (display_aspect > game_aspect)
			new_width = uint32_t(std::round(float(fsr_height) * game_aspect));
		else
			new_height = uint32_t(std::round(float(fsr_width) / game_aspect));

		// This won't preserve the aspect ratio necessarily, but eh.
		if (GSConfig.IntegerScaling)
		{
			new_width -= new_width % vsync.image->get_width();
			new_height -= new_height % vsync.image->get_height();
			if (new_width == 0)
				new_width = fsr_width;
			if (new_height == 0)
				new_height = fsr_height;
		}

		vp_offset_x = std::round(0.5f * float(fsr_width - new_width));
		fsr_width = new_width;
		vp_offset_y = std::round(0.5f * float(fsr_height - new_height));
		fsr_height = new_height;

		// Safeguard against ridiculous situations.
		if (!fsr_width)
			fsr_width = 1;
		if (!fsr_height)
			fsr_height = 1;

		vp_width = float(fsr_width);
		vp_height = float(fsr_height);

		if (GSConfig.LinearPresent == GSPostBilinearMode::BilinearSharp)
		{
			if (!fsr_render_target || fsr_render_target->get_width() != fsr_width || fsr_render_target->get_height() != fsr_height)
			{
				auto fsr_info = ImageCreateInfo::render_target(fsr_width, fsr_height, VK_FORMAT_R8G8B8A8_UNORM);
				fsr_info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
				fsr_info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
				fsr_info.misc |= IMAGE_MISC_MUTABLE_SRGB_BIT;
				fsr_render_target = dev.create_image(fsr_info);
				fsr_render_is_valid = false;
			}
		}
	}

	auto cmd = dev.request_command_buffer();
	// No need to do the upscaling twice when duping frames.
	if (vsync.image && fsr_render_target && !fsr_render_is_valid)
		render_fsr(*cmd, vsync.image->get_view());
	cmd->begin_render_pass(dev.get_swapchain_render_pass(SwapchainRenderPass::ColorOnly));
	if (vsync.image)
	{
		// The RCAS pass is basically free.
		if (fsr_render_target)
			render_rcas(*cmd, fsr_render_target->get_view(), vp_offset_x, vp_offset_y, vp_width, vp_height);
		else
			render_blit(*cmd, vsync.image->get_view(), vp_offset_x, vp_offset_y, vp_width, vp_height);
	}
	cmd->end_render_pass();
	dev.submit(cmd);

	wsi.end_frame();
	has_wsi_begin_frame = false;
	// For pacing purposes.
	has_wsi_begin_frame = wsi.begin_frame();
	has_presented_in_current_swapchain = true;

	PerformanceMetrics::Update(registers_written, stats.num_render_passes != 0, false);
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

bool GSRendererPGS::UpdateWindow()
{
	iface.flush();

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

void GSRendererPGS::ResizeWindow(int width, int height, float /*scale*/)
{
	resize = true;
	window_info.surface_width = width;
	window_info.surface_height = height;
	// TODO: No idea what to do about scale.
}

const WindowInfo &GSRendererPGS::GetWindowInfo() const
{
	return window_info;
}

void GSRendererPGS::SetVSyncMode(GSVSyncMode mode, bool /*allow_present_throttle*/)
{
	if (mode == GSVSyncMode::FIFO)
		wsi.set_present_mode(PresentMode::SyncToVBlank);
	else if (mode == GSVSyncMode::Mailbox)
		wsi.set_present_mode(PresentMode::UnlockedNoTearing);
	else
		wsi.set_present_mode(PresentMode::UnlockedMaybeTear);
	// Unknown what allow_present_throttle means.
}

VkSurfaceKHR GSRendererPGS::create_surface(VkInstance instance, VkPhysicalDevice gpu)
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

void GSRendererPGS::destroy_surface(VkInstance instance, VkSurfaceKHR surface)
{
	WSIPlatform::destroy_surface(instance, surface);
}

std::vector<const char *> GSRendererPGS::get_instance_extensions()
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

std::vector<const char *> GSRendererPGS::get_device_extensions()
{
	return { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
}

bool GSRendererPGS::alive(WSI &)
{
	return true;
}

uint32_t GSRendererPGS::get_surface_width()
{
	return window_info.surface_width;
}

uint32_t GSRendererPGS::get_surface_height()
{
	return window_info.surface_height;
}

void GSRendererPGS::poll_input()
{
	// Dummy, we don't care about input here.
}

void GSRendererPGS::poll_input_async(Granite::InputTrackerHandler *)
{
	// Dummy, we don't care about input here.
}

void GSRendererPGS::event_swapchain_destroyed()
{
	WSIPlatform::event_swapchain_destroyed();
	has_wsi_begin_frame = false;
	has_presented_in_current_swapchain = false;
}

void GSRendererPGS::QueueSnapshot(const std::string &path, u32 gsdump_frames)
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

const VkApplicationInfo *GSRendererPGS::get_application_info()
{
	static const VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr,
	                                       "pcsx2", 0, "Granite", 0, VK_API_VERSION_1_3 };
	return &app;
}
