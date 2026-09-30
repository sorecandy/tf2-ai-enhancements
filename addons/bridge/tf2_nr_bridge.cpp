// tf2-nr-bridge: ReShade add-on that gives DLSS5-Feeder / DLSS 5 neural rendering what Titanfall 2 already
// renders but never exposes: real per-pixel motion vectors, current-frame depth and the frame BEFORE the HUD
// (optionally before tonemapping, in HDR).
//
// 1. Motion vectors. TF2's main forward pass renders three targets at once (found with tf2-frame-probe):
//      RT0 R16G16B16A16 (HDR scene), RT1 R16G16 (velocity), RT2 R8.
//    Every MRT pixel shader writes RT1 = (current_uv - previous_uv) * 1024 (camera AND object motion; the
//    low mantissa bits of .y carry a TSAA flag, <0.4% error). This add-on finds RT1 when that pass is bound,
//    creates an R16G16_FLOAT view of it and binds it to the effect texture semantic "TF2_VELOCITY".
//    TF2_MotionVectors.fx turns it into the feeder's convention (prev_uv = uv + mv): mv = -v / 1024.
//
// 2. Cut point. TF2's engine_post pass (one fullscreen draw, pixel shader with cbuffer CBufEnginePost) reads the
//    HDR scene (t1 FBTexture, scene-linear, before exposure) and writes the tonemapped, graded frame into the
//    back buffer; cockpit/HUD RUI is drawn on top afterwards.
//      HdrCut=0 (LDR): after the engine_post draw, the next immediate-context event runs render_effects() on
//                      views of the back buffer -> NR sees the finished frame without HUD.
//      HdrCut=1 (HDR): at the engine_post draw (before it executes) render_effects() runs on an FP16 RTV of
//                      FBTexture, with our own copy of the current depth and TF2's exposure bound to the effects
//                      (TF2_HDRCut.fx exposes/unexposes around NR). engine_post then tonemaps, grades and adds
//                      bloom on top of the NR result, as in native-DLSS games.
//    render_effects is once-per-frame, so the present-time run is skipped; frames without an engine_post draw
//    (menus, loading) fall back to present time.
//
// 3. Film grain. KillGrain=1 zeroes CBufEnginePost c_noiseOffset/c_noiseScale (bytes 160/164) in the game's own
//    Map/Unmap, so TF2's 24 Hz film grain never reaches the image (it defeats the feeder's hold stabiliser).
//
// 4. Menu. The TF2NR.GraphicsToggles Northstar mod adds "DLSS 5 Neural Rendering" and "DLSS Super Resolution"
//    rows to Settings > Video (convars tf2nr_dlss5_nr / tf2nr_dlss_sr); tf2nr_menu_toggles.hpp reads them.
//
// 5. FG export (0.4, M3). tf2nr_get_hudless (tf2_fg_interop.h): an AddRef'd R8G8B8A8_TYPELESS copy of the back buffer
//    taken right after engine_post and the pre-HUD ReShade effects, before the first HUD draw (LDR cut: after its
//    render_effects; HDR cut: at the first event after the engine_post draw). Two-texture ring, versioned, valid only
//    for the frame it was captured in. Made only on demand (export called in the last 120 frames) or HudlessCopy=2;
//    with DLSS 5 NR off in the menu the bridge then still runs its LDR cut, so no ReShade effect lands on top of the
//    HUD (FSR would treat such an effect delta as UI). Every per-draw callback returns at once while tf2-fg's own native
//    Present runs (tf2fg_in_native_present()).
//
// 6. 0.4.1 (M3 review fixes): only the game's D3D11 device is served (renodx-dlss5's private D3D12 device, created through
//    ReShade's hooked D3D12CreateDevice, could raise events here: a D3D12 event between engine_post and the next D3D11
//    command used to cancel that frame's cut); the present callback also returns while tf2-fg's native Present runs (as
//    tf2-dlss-sr's does, so both exporters' frame counters advance together); the HUD-less snapshot is invalidated at the
//    engine_post draw itself when no copy follows (the header's "missed capture point invalidates immediately"); the ring
//    is only used for a single-sample, single-layer back buffer.
//
// Config: ReShade.ini [TF2NRBridge]  MotionVectors=1 PreHUD=1 HdrCut=0 KillGrain=1 HudlessCopy=1 (0 never, 1 on demand, 2 always)
//         ToggleKey=119 (F8, PreHUD on/off)  HdrKey=118 (F7, HdrCut on/off)  CaptureKey=120 (F9, flicker capture)
//         CaptureFrames=12 CaptureCrop=0 (1 = centre half)  DumpKey=0 (velocity dumps, off by default)
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d11.h>
#include <reshade.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include "tf2nr_menu_toggles.hpp"
#include "tf2_fg_interop.h"

using namespace reshade::api;

extern "C" __declspec(dllexport) const char *NAME = "TF2 NR Bridge";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Titanfall 2: engine motion vectors, current depth, pre-HUD / pre-tonemap effect rendering and Video-menu toggles for DLSS5-Feeder.";

namespace
{
	std::recursive_mutex g_mutex;
	FILE *g_log = nullptr;
	std::filesystem::path g_dir;

	// config
	bool g_cfg_mv = true, g_cfg_prehud = true, g_cfg_hdrcut = false, g_cfg_kill_grain = true;
	uint64_t g_cfg_composite_ps = 0xddc623bf7e8ea9f6ull;
	int g_cfg_toggle_key = VK_F8, g_cfg_hdr_key = VK_F7, g_cfg_dump_key = 0, g_cfg_capture_key = VK_F9;
	int g_cfg_dump_burst = 12, g_cfg_dump_interval = 5, g_cfg_capture_frames = 12;
	bool g_cfg_capture_crop = false;
	int g_cfg_hudless = 1;                                       // HudlessCopy: 0 never, 1 on demand (export called), 2 always

	// tf2-fg link: callbacks raised while tf2-fg's own native Present runs on this thread come from a third-party present
	// hook rendering through ReShade's proxy mid-frame -> ignored (destroy events excepted). Resolved once per frame.
	std::atomic<PFN_tf2fg_in_native_present> g_fg_native { nullptr };
	HMODULE g_fg_mod = nullptr;
	std::atomic<uint64_t> g_fg_skips { 0 };
	inline bool fg_native()
	{
		const PFN_tf2fg_in_native_present f = g_fg_native.load(std::memory_order_relaxed);
		return f != nullptr && f();
	}
#define BR_SKIP_FG(...) if (fg_native()) { g_fg_skips.fetch_add(1, std::memory_order_relaxed); return __VA_ARGS__; }

	// 0.4.1: only the game's D3D11 device (learned at its first present; D3D11 only before that). renodx-dlss5 creates a
	// private D3D12 device through ReShade's hook, whose command-list events would otherwise reach these callbacks.
	std::atomic<device *> g_game_dev { nullptr };
	std::atomic<uint64_t> g_foreign_events { 0 };
	inline bool foreign_dev(device *d)
	{
		if (d == nullptr) return true;
		device *const g = g_game_dev.load(std::memory_order_relaxed);
		const bool f = g != nullptr ? d != g : d->get_api() != device_api::d3d11;
		if (f) g_foreign_events.fetch_add(1, std::memory_order_relaxed);
		return f;
	}
	inline bool foreign_cmd(command_list *cmd) { return cmd == nullptr || foreign_dev(cmd->get_device()); }

	// effect runtime + back-buffer views (D3D11 exposes one back buffer)
	effect_runtime *g_runtime = nullptr;
	device *g_device = nullptr;
	resource g_bb = { 0 };
	resource_view g_bb_rtv = { 0 }, g_bb_rtv_srgb = { 0 };

	// view -> resource cache
	std::unordered_map<uint64_t, resource_desc> g_view_desc;
	std::unordered_map<uint64_t, uint64_t> g_view_res;

	// velocity
	resource g_velocity = { 0 };
	resource_view g_velocity_srv = { 0 };

	// bound state on the immediate context (tracked from events)
	std::unordered_set<uint64_t> g_composite_pipelines;         // engine_post pixel-shader pipelines
	uint64_t g_bound_ps = 0, g_bound_rt0_res = 0, g_bound_dsv_res = 0;
	resource_view g_ps_srv[32] = {};                             // pixel-stage SRVs by slot

	// pre-HUD (LDR) / HDR cut
	bool g_pending = false;                                     // engine_post drawn, post-composite hook not yet run
	bool g_hdr_ran_this_frame = false;
	bool g_hdr_active = false;                                  // inside the HDR cut's render_effects
	bool g_in_render = false;
	resource g_hdr_fb = { 0 };
	resource_view g_hdr_rtv = { 0 };
	resource g_depth_copy = { 0 }, g_expo_copy = { 0 };
	resource_view g_depth_srv = { 0 }, g_expo_srv = { 0 };
	bool g_cut_active = false;                                  // inside a cut's render_effects: our DEPTH copy is bound
	bool g_hdr_logged = false;
	resource_view g_bound_rtv0_view = { 0 }, g_bound_dsv_view = { 0 };

	// film grain
	uint64_t g_epost_cb = 0, g_epost_mapped = 0;
	uint8_t *g_epost_ptr = nullptr;
	uint64_t g_grain_patches = 0;

	// stats
	uint64_t g_frame = 0, g_main_binds = 0, g_prehud_frames = 0, g_hdr_frames = 0, g_composites = 0, g_skipped = 0;
	uint64_t g_window_prehud = 0, g_window_hdr = 0, g_window_frames = 0;
	bool g_key_toggle_down = false, g_key_hdr_down = false, g_key_dump_down = false, g_key_capture_down = false;
	int g_dump_index = 0, g_dump_left = 0, g_dump_wait = 0;

	// flicker capture: N consecutive cut frames; in/out = the effect target before/after render_effects (the back
	// buffer in LDR mode, FBTexture FP16 in HDR mode), fin = the back buffer after engine_post (HDR mode), vel =
	// TF2's velocity target. Copied to readback textures, written to disk once the burst is complete.
	struct cap_frame { resource in = { 0 }, out = { 0 }, fin = { 0 }, vel = { 0 }; uint64_t frame = 0; bool hdr = false; };
	std::vector<cap_frame> g_cap;
	int g_cap_next = -1, g_cap_burst = 0;
	bool g_cap_hdr = false;
	subresource_box g_cap_box = {};

	void logf(const char *fmt, ...)
	{
		if (!g_log) return;
		SYSTEMTIME t; GetLocalTime(&t);
		fprintf(g_log, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
		va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
		fputc('\n', g_log); fflush(g_log);
	}

	uint64_t fnv1a(const void *data, size_t size)
	{
		uint64_t h = 1469598103934665603ull;
		const uint8_t *p = static_cast<const uint8_t *>(data);
		for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 1099511628211ull; }
		return h;
	}

	bool has_str(const void *p, size_t n, const char *s)
	{
		const size_t m = strlen(s);
		const char *b = static_cast<const char *>(p);
		for (size_t i = 0; i + m <= n; ++i)
			if (memcmp(b + i, s, m) == 0) return true;
		return false;
	}

	void load_config()
	{
		reshade::get_config_value(nullptr, "TF2NRBridge", "MotionVectors", g_cfg_mv);
		reshade::get_config_value(nullptr, "TF2NRBridge", "PreHUD", g_cfg_prehud);
		reshade::get_config_value(nullptr, "TF2NRBridge", "HdrCut", g_cfg_hdrcut);
		reshade::get_config_value(nullptr, "TF2NRBridge", "KillGrain", g_cfg_kill_grain);
		reshade::get_config_value(nullptr, "TF2NRBridge", "ToggleKey", g_cfg_toggle_key);
		reshade::get_config_value(nullptr, "TF2NRBridge", "HdrKey", g_cfg_hdr_key);
		reshade::get_config_value(nullptr, "TF2NRBridge", "DumpKey", g_cfg_dump_key);
		reshade::get_config_value(nullptr, "TF2NRBridge", "DumpBurst", g_cfg_dump_burst);
		reshade::get_config_value(nullptr, "TF2NRBridge", "DumpInterval", g_cfg_dump_interval);
		reshade::get_config_value(nullptr, "TF2NRBridge", "CaptureKey", g_cfg_capture_key);
		reshade::get_config_value(nullptr, "TF2NRBridge", "CaptureFrames", g_cfg_capture_frames);
		reshade::get_config_value(nullptr, "TF2NRBridge", "CaptureCrop", g_cfg_capture_crop);
		reshade::get_config_value(nullptr, "TF2NRBridge", "HudlessCopy", g_cfg_hudless);
		g_cfg_capture_frames = std::clamp(g_cfg_capture_frames, 2, 60);
		g_cfg_hudless = std::clamp(g_cfg_hudless, 0, 2);
		char buf[64] = {}; size_t len = sizeof(buf);
		if (reshade::get_config_value(nullptr, "TF2NRBridge", "CompositePS", buf, &len) && buf[0] != '\0')
			g_cfg_composite_ps = strtoull(buf, nullptr, 16);
		logf("config: MotionVectors=%d PreHUD=%d HdrCut=%d KillGrain=%d ToggleKey=%d HdrKey=%d CaptureKey=%d CaptureFrames=%d CaptureCrop=%d HudlessCopy=%d",
			g_cfg_mv, g_cfg_prehud, g_cfg_hdrcut, g_cfg_kill_grain, g_cfg_toggle_key, g_cfg_hdr_key, g_cfg_capture_key, g_cfg_capture_frames, g_cfg_capture_crop, g_cfg_hudless);
	}

	bool is_rgba16(format f) { return f == format::r16g16b16a16_typeless || f == format::r16g16b16a16_float; }
	bool is_rg16(format f) { return f == format::r16g16_typeless || f == format::r16g16_float; }

	const resource_desc *desc_of_view(device *dev, resource_view v, uint64_t *res_out)
	{
		auto it = g_view_desc.find(v.handle);
		if (it == g_view_desc.end())
		{
			const resource r = dev->get_resource_from_view(v);
			if (r.handle == 0) return nullptr;
			it = g_view_desc.emplace(v.handle, dev->get_resource_desc(r)).first;
			g_view_res[v.handle] = r.handle;
		}
		*res_out = g_view_res[v.handle];
		return &it->second;
	}

	uint64_t res_of_view(device *dev, resource_view v)
	{
		if (v.handle == 0) return 0;
		uint64_t r = 0;
		return desc_of_view(dev, v, &r) != nullptr ? r : 0;
	}

	void destroy_view(resource_view &v) { if (v.handle != 0 && g_device != nullptr) g_device->destroy_resource_view(v); v = { 0 }; }
	void destroy_res(resource &r) { if (r.handle != 0 && g_device != nullptr) g_device->destroy_resource(r); r = { 0 }; }

	// ---------------- motion vectors ----------------

	void release_velocity(const char *why)
	{
		if (g_velocity_srv.handle != 0 && g_runtime != nullptr)
			g_runtime->update_texture_bindings("TF2_VELOCITY", resource_view { 0 }, resource_view { 0 });
		destroy_view(g_velocity_srv);
		if (g_velocity.handle != 0)
			logf("velocity target %llx released (%s)", (unsigned long long)g_velocity.handle, why);
		g_velocity = { 0 };
	}

	void adopt_velocity(device *dev, uint64_t res, const resource_desc &d)
	{
		release_velocity("replaced");
		g_device = dev;
		g_velocity = { res };
		if (!dev->create_resource_view(g_velocity, resource_usage::shader_resource, resource_view_desc(format::r16g16_float), &g_velocity_srv))
		{
			logf("velocity target %llx %ux%u: create SRV FAILED", (unsigned long long)res, d.texture.width, d.texture.height);
			g_velocity_srv = { 0 };
			return;
		}
		if (g_runtime != nullptr)
			g_runtime->update_texture_bindings("TF2_VELOCITY", g_velocity_srv, g_velocity_srv);
		logf("velocity target adopted: %llx %ux%u fmt=%u -> SRV %llx bound to TF2_VELOCITY",
			(unsigned long long)res, d.texture.width, d.texture.height, (unsigned)d.texture.format, (unsigned long long)g_velocity_srv.handle);
	}

	// Copies the velocity target to a readback texture and writes it as raw half floats (header: 'TF2V', w, h, row pitch).
	void dump_velocity(command_list *cmd)
	{
		if (g_velocity.handle == 0 || g_device == nullptr) { logf("dump: no velocity target"); return; }
		const resource_desc sd = g_device->get_resource_desc(g_velocity);
		const resource_desc rd(sd.texture.width, sd.texture.height, 1, 1, format::r16g16_float, 1, memory_heap::readback, resource_usage::copy_dest);
		resource staging = { 0 };
		if (!g_device->create_resource(rd, nullptr, resource_usage::copy_dest, &staging)) { logf("dump: create readback FAILED"); return; }
		cmd->copy_texture_region(g_velocity, 0, nullptr, staging, 0, nullptr);
		subresource_data data = {};
		if (g_device->map_texture_region(staging, 0, nullptr, map_access::read_only, &data))
		{
			wchar_t name[64]; swprintf(name, 64, L"tf2-nr-bridge-velocity_%03d.bin", ++g_dump_index);
			if (FILE *f = _wfopen((g_dir / name).c_str(), L"wb"))
			{
				const uint32_t hdr[4] = { 0x56324654u, sd.texture.width, sd.texture.height, sd.texture.width * 4u };
				fwrite(hdr, sizeof(hdr), 1, f);
				for (uint32_t y = 0; y < sd.texture.height; ++y)
					fwrite(static_cast<const uint8_t *>(data.data) + size_t(y) * data.row_pitch, 4, sd.texture.width, f);
				fclose(f);
				logf("dump: wrote %ls (frame %llu)", name, (unsigned long long)g_frame);
			}
			g_device->unmap_texture_region(staging, 0);
		}
		else
			logf("dump: map FAILED");
		g_device->destroy_resource(staging);
	}

	// ---------------- flicker capture ----------------

	void release_capture()
	{
		for (cap_frame &c : g_cap)
		{
			destroy_res(c.in); destroy_res(c.out); destroy_res(c.fin); destroy_res(c.vel);
		}
		g_cap.clear();
		g_cap_next = -1;
	}

	uint32_t bytes_per_pixel(format f)
	{
		switch (f)
		{
		case format::r16g16b16a16_float: case format::r16g16b16a16_typeless: return 8;
		default: return 4;
		}
	}

	bool make_readback(const resource_desc &src, resource &out)
	{
		const uint32_t w = g_cap_box.width(), h = g_cap_box.height();
		const resource_desc d(w, h, 1, 1, format_to_default_typed(src.texture.format), 1, memory_heap::readback, resource_usage::copy_dest);
		return g_device->create_resource(d, nullptr, resource_usage::copy_dest, &out);
	}

	bool arm_capture()
	{
		if (g_device == nullptr || g_bb.handle == 0) { logf("capture: no back buffer yet"); return false; }
		release_capture();
		const resource_desc bd = g_device->get_resource_desc(g_bb);
		const uint32_t W = bd.texture.width, H = bd.texture.height;
		g_cap_box = g_cfg_capture_crop ? subresource_box { W / 4, H / 4, 0, W / 4 + W / 2, H / 4 + H / 2, 1 } : subresource_box { 0, 0, 0, W, H, 1 };
		g_cap_hdr = g_cfg_hdrcut && g_hdr_fb.handle != 0;
		const resource_desc sd = g_cap_hdr ? g_device->get_resource_desc(g_hdr_fb) : bd;
		g_cap.resize(g_cfg_capture_frames);
		for (cap_frame &c : g_cap)
		{
			c.hdr = g_cap_hdr;
			bool ok = make_readback(sd, c.in) && make_readback(sd, c.out);
			if (ok && g_cap_hdr) ok = make_readback(bd, c.fin);
			if (ok && g_velocity.handle != 0) ok = make_readback(g_device->get_resource_desc(g_velocity), c.vel);
			if (!ok) { logf("capture: create readback textures FAILED"); release_capture(); return false; }
		}
		g_cap_next = 0;
		++g_cap_burst;
		logf("capture %d: armed for %d consecutive cut frames (%s mode, box %u,%u %ux%u)", g_cap_burst, g_cfg_capture_frames,
			g_cap_hdr ? "HDR" : "LDR", g_cap_box.left, g_cap_box.top, g_cap_box.width(), g_cap_box.height());
		return true;
	}

	void cap_copy(command_list *cmd, resource src, resource dst)
	{
		if (src.handle != 0 && dst.handle != 0)
			cmd->copy_texture_region(src, 0, &g_cap_box, dst, 0, nullptr);
	}

	// Raw dump: header { 'TF2C', width, height, format } then width*bpp bytes per row.
	void write_raw(resource r, const wchar_t *name)
	{
		if (r.handle == 0) return;
		const resource_desc d = g_device->get_resource_desc(r);
		const uint32_t bpp = bytes_per_pixel(d.texture.format);
		subresource_data data = {};
		if (!g_device->map_texture_region(r, 0, nullptr, map_access::read_only, &data)) { logf("capture: map FAILED (%ls)", name); return; }
		if (FILE *f = _wfopen((g_dir / name).c_str(), L"wb"))
		{
			const uint32_t hdr[4] = { 0x43324654u, d.texture.width, d.texture.height, (uint32_t)d.texture.format };
			fwrite(hdr, sizeof(hdr), 1, f);
			for (uint32_t y = 0; y < d.texture.height; ++y)
				fwrite(static_cast<const uint8_t *>(data.data) + size_t(y) * data.row_pitch, bpp, d.texture.width, f);
			fclose(f);
		}
		g_device->unmap_texture_region(r, 0);
	}

	void write_capture()
	{
		wchar_t name[96];
		for (size_t i = 0; i < g_cap.size(); ++i)
		{
			const cap_frame &c = g_cap[i];
			swprintf(name, 96, L"tf2-nr-bridge-cap%02d_%02zu_in.bin", g_cap_burst, i);  write_raw(c.in, name);
			swprintf(name, 96, L"tf2-nr-bridge-cap%02d_%02zu_out.bin", g_cap_burst, i); write_raw(c.out, name);
			swprintf(name, 96, L"tf2-nr-bridge-cap%02d_%02zu_fin.bin", g_cap_burst, i); write_raw(c.fin, name);
			swprintf(name, 96, L"tf2-nr-bridge-cap%02d_%02zu_vel.bin", g_cap_burst, i); write_raw(c.vel, name);
		}
		logf("capture %d: wrote %zu frames (game frames %llu..%llu, %s, NR menu=%d)", g_cap_burst, g_cap.size(),
			(unsigned long long)g_cap.front().frame, (unsigned long long)g_cap.back().frame, g_cap_hdr ? "HDR" : "LDR", (int)tf2nr::g_nr_enabled);
		release_capture();
	}

	// ---------------- back buffer views ----------------

	void release_bb_views()
	{
		destroy_view(g_bb_rtv); destroy_view(g_bb_rtv_srgb);
		g_bb = { 0 };
	}

	// Views of the back buffer exactly as ReShade builds them for present (runtime.cpp: UNORM rtv + UNORM_SRGB rtv_srgb).
	bool ensure_bb_views()
	{
		if (g_runtime == nullptr) return false;
		const resource bb = g_runtime->get_current_back_buffer();
		if (bb.handle == g_bb.handle && g_bb_rtv.handle != 0) return true;
		device *dev = g_runtime->get_device();
		g_device = dev;
		release_bb_views();
		const resource_desc d = dev->get_resource_desc(bb);
		const bool ok1 = dev->create_resource_view(bb, resource_usage::render_target,
			resource_view_desc(resource_view_type::texture_2d, format_to_default_typed(d.texture.format, 0), 0, 1, 0, 1), &g_bb_rtv);
		const bool ok2 = dev->create_resource_view(bb, resource_usage::render_target,
			resource_view_desc(resource_view_type::texture_2d, format_to_default_typed(d.texture.format, 1), 0, 1, 0, 1), &g_bb_rtv_srgb);
		if (!ok1 || !ok2)
		{
			logf("back buffer %llx: create RTVs FAILED (%d %d)", (unsigned long long)bb.handle, ok1, ok2);
			release_bb_views();
			return false;
		}
		g_bb = bb;
		logf("back buffer %llx %ux%u fmt=%u: rtv %llx rtv_srgb %llx", (unsigned long long)bb.handle, d.texture.width, d.texture.height,
			(unsigned)d.texture.format, (unsigned long long)g_bb_rtv.handle, (unsigned long long)g_bb_rtv_srgb.handle);
		return true;
	}

	// ---------------- HDR cut ----------------

	void release_hdr()
	{
		if (g_runtime != nullptr && g_expo_srv.handle != 0)
			g_runtime->update_texture_bindings("TF2_EXPOSURE", resource_view { 0 }, resource_view { 0 });
		destroy_view(g_hdr_rtv); g_hdr_fb = { 0 };
		destroy_view(g_depth_srv); destroy_res(g_depth_copy);
		destroy_view(g_expo_srv); destroy_res(g_expo_copy);
	}

	// Our own copy of a source texture (same size/format family), with an SRV of the given typed format.
	bool make_copy_target(device *dev, resource src, format tex_fmt, format srv_fmt, resource &out, resource_view &out_srv)
	{
		const resource_desc sd = dev->get_resource_desc(src);
		const resource_desc d(sd.texture.width, sd.texture.height, 1, 1, tex_fmt, 1, memory_heap::default_, resource_usage::shader_resource | resource_usage::copy_dest);
		if (!dev->create_resource(d, nullptr, resource_usage::shader_resource, &out)) return false;
		if (!dev->create_resource_view(out, resource_usage::shader_resource, resource_view_desc(srv_fmt), &out_srv)) { dev->destroy_resource(out); out = { 0 }; return false; }
		return true;
	}

	// Our own copy of the main depth-stencil (R24G8), bound to DEPTH while a cut renders the effects.
	bool ensure_depth_copy(device *dev, resource dsv)
	{
		if (dsv.handle == 0) return false;
		const resource_desc dd = dev->get_resource_desc(dsv);
		if (g_depth_copy.handle != 0)
		{
			const resource_desc cd = dev->get_resource_desc(g_depth_copy);
			if (cd.texture.width == dd.texture.width && cd.texture.height == dd.texture.height) return true;
		}
		destroy_view(g_depth_srv); destroy_res(g_depth_copy);
		if (dd.texture.format != format::r24_g8_typeless && dd.texture.format != format::d24_unorm_s8_uint)
		{
			logf("depth: main depth format %u unsupported", (unsigned)dd.texture.format);
			return false;
		}
		if (!make_copy_target(dev, dsv, format::r24_g8_typeless, format::r24_unorm_x8_uint, g_depth_copy, g_depth_srv))
		{
			logf("depth: copy texture FAILED");
			return false;
		}
		logf("depth: own copy %llx (SRV %llx) of main DSV %llx %ux%u", (unsigned long long)g_depth_copy.handle, (unsigned long long)g_depth_srv.handle,
			(unsigned long long)dsv.handle, dd.texture.width, dd.texture.height);
		return true;
	}

	// After render_effects: ReShade restored the app state, but FBTexture was bound as a render target in between, which
	// makes D3D11 drop it from PS slot 1 if the SRV came back before the RTVs. Re-assert OM + t1 on the native context.
	void rebind_engine_post_inputs(command_list *cmd)
	{
		auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native());
		if (ctx == nullptr) return;
		ID3D11RenderTargetView *rtv = reinterpret_cast<ID3D11RenderTargetView *>(g_bound_rtv0_view.handle);
		ID3D11DepthStencilView *dsv = reinterpret_cast<ID3D11DepthStencilView *>(g_bound_dsv_view.handle);
		ctx->OMSetRenderTargets(1, &rtv, dsv);
		ID3D11ShaderResourceView *srv = reinterpret_cast<ID3D11ShaderResourceView *>(g_ps_srv[1].handle);
		ctx->PSSetShaderResources(1, 1, &srv);
	}

	bool ensure_hdr_resources(device *dev, resource fb, resource dsv, resource expo)
	{
		if (fb.handle != g_hdr_fb.handle)
		{
			destroy_view(g_hdr_rtv);
			if (!dev->create_resource_view(fb, resource_usage::render_target, resource_view_desc(format::r16g16b16a16_float), &g_hdr_rtv))
			{
				logf("HDR cut: FP16 RTV of FBTexture %llx FAILED", (unsigned long long)fb.handle);
				g_hdr_fb = { 0 };
				return false;
			}
			g_hdr_fb = fb;
			const resource_desc fd = dev->get_resource_desc(fb);
			logf("HDR cut: FBTexture %llx %ux%u fmt=%u -> RTV %llx", (unsigned long long)fb.handle, fd.texture.width, fd.texture.height,
				(unsigned)fd.texture.format, (unsigned long long)g_hdr_rtv.handle);
		}
		if (!ensure_depth_copy(dev, dsv)) return false;
		if (g_expo_copy.handle == 0 && expo.handle != 0)
		{
			const format ef = dev->get_resource_desc(expo).texture.format;
			if (!make_copy_target(dev, expo, format_to_typeless(ef), format_to_default_typed(ef), g_expo_copy, g_expo_srv))
			{
				logf("HDR cut: exposure copy texture FAILED (fmt %u)", (unsigned)ef);
				return false;
			}
			g_runtime->update_texture_bindings("TF2_EXPOSURE", g_expo_srv, g_expo_srv);
			logf("HDR cut: exposure copy %llx (fmt %u) bound to TF2_EXPOSURE", (unsigned long long)g_expo_copy.handle, (unsigned)ef);
		}
		return g_expo_copy.handle != 0;
	}

	// At the engine_post draw, before it executes. Returns true when the effects ran on the HDR scene.
	bool run_hdr_cut(command_list *cmd)
	{
		device *dev = cmd->get_device();
		const resource fb = { res_of_view(dev, g_ps_srv[1]) };
		const resource expo = { res_of_view(dev, g_ps_srv[30]) };
		const resource dsv = { g_bound_dsv_res };
		if (fb.handle == 0 || dsv.handle == 0) return false;
		const resource_desc fd = dev->get_resource_desc(fb), bd = dev->get_resource_desc(g_bb);
		if (!is_rgba16(fd.texture.format) || fd.texture.width != bd.texture.width || fd.texture.height != bd.texture.height)
		{
			if (!g_hdr_logged) { g_hdr_logged = true; logf("HDR cut: t1 %llx is %ux%u fmt=%u, not a full-size FP16 scene - using the LDR cut", (unsigned long long)fb.handle, fd.texture.width, fd.texture.height, (unsigned)fd.texture.format); }
			return false;
		}
		if (!ensure_hdr_resources(dev, fb, dsv, expo)) return false;

		cmd->copy_resource(dsv, g_depth_copy);                   // current frame, world + viewmodel, not yet cleared
		if (expo.handle != 0) cmd->copy_resource(expo, g_expo_copy);

		const bool cap = g_cap_next >= 0 && g_cap_next < static_cast<int>(g_cap.size()) && g_cap_hdr;
		if (cap) cap_copy(cmd, g_hdr_fb, g_cap[g_cap_next].in);
		g_hdr_active = true; g_cut_active = true;
		g_in_render = true;
		g_runtime->render_effects(cmd, g_hdr_rtv, g_hdr_rtv);   // ReShade restores the app state afterwards
		g_in_render = false;
		g_hdr_active = false; g_cut_active = false;
		rebind_engine_post_inputs(cmd);
		if (cap)
		{
			cap_frame &c = g_cap[g_cap_next];
			cap_copy(cmd, g_hdr_fb, c.out);
			cap_copy(cmd, g_velocity, c.vel);
			c.frame = g_frame;                                    // 'fin' is taken after engine_post, then the index advances
		}
		g_hdr_ran_this_frame = true;
		++g_hdr_frames; ++g_window_hdr;
		if (g_hdr_frames == 1)
			logf("HDR cut: first effects run on the pre-tonemap scene (frame %llu)", (unsigned long long)g_frame);
		return true;
	}

	// HdrCut is only safe when TF2_HDRCut.fx's expose/unexpose techniques bracket the NR chain; otherwise NR would get
	// unexposed scene values and engine_post would lose the cloak alpha (the feeder's blit writes alpha = 1).
	bool hdr_bracket_enabled()
	{
		const effect_technique a = g_runtime->find_technique("TF2_HDRCut.fx", "TF2_HDR_Expose");
		const effect_technique b = g_runtime->find_technique("TF2_HDRCut.fx", "TF2_HDR_Unexpose");
		const bool ok = a.handle != 0 && b.handle != 0 && g_runtime->get_technique_state(a) && g_runtime->get_technique_state(b);
		static bool logged = false;
		if (!ok && !logged) { logged = true; logf("HDR cut: TF2_HDR_Expose/TF2_HDR_Unexpose not loaded+enabled in the preset - staying on the LDR cut"); }
		if (ok) logged = false;
		return ok;
	}

	// ---------------- HUD-less copy for FG (0.4) ----------------
	// Written on the render thread (under g_mutex), read by the export from any thread: hl::mx is the only lock the export
	// takes, and our side always takes it after g_mutex.
	namespace hl
	{
		std::mutex mx;
		ID3D11Texture2D *tex[2] = {};                           // R8G8B8A8_TYPELESS ring (native, our device)
		uint32_t w = 0, h = 0, bb_fmt = 0;
		uint64_t generation = 1, serial = 0;
		int next = 0;
		uint64_t retry_frame = 0;                               // creation failure latch
		bool fmt_logged = false;
		struct snap_t { bool valid = false; uint64_t frame = 0, serial = 0, generation = 0; int ring = 0; uint32_t flags = 0, w = 0, h = 0, bb_fmt = 0; } s;
		std::atomic<uint64_t> frame_now { 0 };                 // mirror of g_frame
		std::atomic<uint64_t> demand_frame { 0 };
		std::atomic<bool> demanded { false };
		std::atomic<uint64_t> calls { 0 }, calls_valid { 0 }, calls_bad { 0 };
		uint64_t win_captures = 0, win_ldr = 0, win_hdr = 0, win_fgcut = 0, win_fail = 0, win_valid_frames = 0;

		void release()   // render thread; under the export lock so the export never AddRefs a dying texture
		{
			std::lock_guard lk(mx);
			for (ID3D11Texture2D *&t : tex) { if (t) { t->Release(); t = nullptr; } }
			w = h = bb_fmt = 0; next = 0;
			s.valid = false;
			++generation;
		}
	}

	bool hudless_wanted()
	{
		if (g_cfg_hudless <= 0) return false;
		if (g_cfg_hudless >= 2) return true;
		if (!hl::demanded.load()) return false;
		return g_frame <= hl::demand_frame.load() + 120;
	}

	// (Re)create the ring for the current back buffer. False = no copy this frame.
	bool ensure_hudless()
	{
		if (g_device == nullptr || g_bb.handle == 0) return false;
		const resource_desc bd = g_device->get_resource_desc(g_bb);
		const uint32_t f = static_cast<uint32_t>(bd.texture.format);   // reshade::api::format == DXGI_FORMAT numbering
		if (f != DXGI_FORMAT_R8G8B8A8_TYPELESS && f != DXGI_FORMAT_R8G8B8A8_UNORM && f != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
		{
			if (!hl::fmt_logged) { hl::fmt_logged = true; logf("hudless: back buffer format %u is not in the R8G8B8A8 family - no HUD-less copies", f); }
			return false;
		}
		// 0.4.1: CopyResource into the ring needs identical sample count / layers / mips (a multisampled back buffer would be
		// dropped silently by D3D11 and a stale ring texture published as VALID)
		if (bd.type != resource_type::texture_2d || bd.texture.samples != 1 || bd.texture.depth_or_layers != 1 || bd.texture.levels != 1)
		{
			if (!hl::fmt_logged)
			{
				hl::fmt_logged = true;
				logf("hudless: back buffer has %u samples, %u layers, %u mips (1/1/1 needed) - no HUD-less copies", (unsigned)bd.texture.samples,
					(unsigned)bd.texture.depth_or_layers, (unsigned)bd.texture.levels);
			}
			return false;
		}
		if (hl::tex[0] && hl::tex[1] && hl::w == bd.texture.width && hl::h == bd.texture.height && hl::bb_fmt == f) return true;
		if (g_frame < hl::retry_frame) return false;
		hl::release();
		auto *dev = reinterpret_cast<ID3D11Device *>(g_device->get_native());
		if (dev == nullptr) return false;
		D3D11_TEXTURE2D_DESC d = {};
		d.Width = bd.texture.width; d.Height = bd.texture.height; d.MipLevels = 1; d.ArraySize = 1;
		d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		ID3D11Texture2D *t[2] = {};
		const bool ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &t[0])) && t[0] && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &t[1])) && t[1];
		if (!ok)
		{
			for (ID3D11Texture2D *x : t) if (x) x->Release();
			hl::retry_frame = g_frame + 600;
			logf("hudless: creating 2x %ux%u R8G8B8A8_TYPELESS FAILED - retry in 600 frames", d.Width, d.Height);
			return false;
		}
		{
			std::lock_guard lk(hl::mx);
			hl::tex[0] = t[0]; hl::tex[1] = t[1];
			hl::w = d.Width; hl::h = d.Height; hl::bb_fmt = f; hl::next = 0;
			++hl::generation;
		}
		logf("hudless: ring of 2 x %ux%u R8G8B8A8_TYPELESS created (back buffer fmt %u), generation %llu", d.Width, d.Height, f, (unsigned long long)hl::generation);
		return true;
	}

	// Copy the back buffer (engine_post done, pre-HUD effects done, no HUD yet) into the ring. Native CopyResource: no
	// ReShade events for the other add-ons.
	void hudless_capture(command_list *cmd, uint32_t flags)
	{
		if (!hudless_wanted()) return;
		auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native());
		if (ctx == nullptr || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) { ++hl::win_fail; return; }
		if (!ensure_hudless()) { ++hl::win_fail; return; }
		const int i = hl::next;
		ctx->CopyResource(hl::tex[i], reinterpret_cast<ID3D11Resource *>(g_bb.handle));
		hl::next = i ^ 1;
		{
			std::lock_guard lk(hl::mx);
			hl::s.valid = true; hl::s.frame = g_frame; hl::s.serial = ++hl::serial; hl::s.generation = hl::generation;
			hl::s.ring = i; hl::s.flags = flags; hl::s.w = hl::w; hl::s.h = hl::h; hl::s.bb_fmt = hl::bb_fmt;
		}
		++hl::win_captures;
		if (flags & TF2NR_HUDLESS_HDR_CUT) ++hl::win_hdr; else ++hl::win_ldr;
		if (flags & TF2NR_HUDLESS_CUT_FOR_FG) ++hl::win_fgcut;
		if (hl::serial == 1) logf("hudless: first copy at frame %llu (flags 0x%x)", (unsigned long long)g_frame, flags);
	}

	bool g_cut_for_fg = false;                                  // this frame's LDR cut runs only because FG wants the copy

	// ---------------- LDR pre-HUD cut ----------------

	void run_ldr_cut(command_list *cmd, const char *where)
	{
		const bool cap = g_cap_next >= 0 && g_cap_next < static_cast<int>(g_cap.size()) && !g_cap_hdr;
		if (cap) cap_copy(cmd, g_bb, g_cap[g_cap_next].in);
		device *dev = cmd->get_device();
		const resource dsv = { g_bound_dsv_res };               // the main DSV, still bound from engine_post, not yet cleared
		const bool own_depth = ensure_depth_copy(dev, dsv);
		if (own_depth) cmd->copy_resource(dsv, g_depth_copy);
		g_cut_active = own_depth;
		g_in_render = true;
		g_runtime->render_effects(cmd, g_bb_rtv, g_bb_rtv_srgb);
		g_in_render = false;
		g_cut_active = false;
		if (cap)
		{
			cap_frame &c = g_cap[g_cap_next++];
			cap_copy(cmd, g_bb, c.out);
			cap_copy(cmd, g_velocity, c.vel);
			c.frame = g_frame;
		}
		++g_prehud_frames; ++g_window_prehud;
		if (g_prehud_frames == 1)
			logf("pre-HUD: first effects run at '%s' after engine_post (frame %llu)", where, (unsigned long long)g_frame);
		hudless_capture(cmd, TF2NR_HUDLESS_LDR_CUT | TF2NR_HUDLESS_EFFECTS_PREHUD | (tf2nr::g_nr_enabled ? TF2NR_HUDLESS_NR_ON : 0u) |
			(g_cut_for_fg ? TF2NR_HUDLESS_CUT_FOR_FG : 0u));
	}

	// First immediate-context event after the engine_post draw (the frame is tonemapped, no HUD yet).
	void run_post_composite(command_list *cmd, const char *where)
	{
		g_pending = false;
		if (g_in_render || g_runtime == nullptr) return;
		if (cmd != g_runtime->get_command_queue()->get_immediate_command_list()) { ++g_skipped; return; }
		if (!ensure_bb_views()) { ++g_skipped; return; }
		if (g_hdr_ran_this_frame)
		{
			// HDR mode: effects already ran; just grab the final (tonemapped, HUD-free) frame for the capture
			if (g_cap_next >= 0 && g_cap_next < static_cast<int>(g_cap.size()) && g_cap_hdr)
				cap_copy(cmd, g_bb, g_cap[g_cap_next++].fin);
			hudless_capture(cmd, TF2NR_HUDLESS_HDR_CUT | TF2NR_HUDLESS_EFFECTS_PREHUD | TF2NR_HUDLESS_NR_ON);
			return;
		}
		run_ldr_cut(cmd, where);
	}

	void check_pending(command_list *cmd, const char *where)
	{
		if (g_pending)
			run_post_composite(cmd, where);
	}

	// ---------------- events ----------------

	void on_init_effect_runtime(effect_runtime *rt)
	{
		std::lock_guard lock(g_mutex);
		g_runtime = rt;
		g_device = rt->get_device();
		load_config();
		if (g_velocity_srv.handle != 0)
			rt->update_texture_bindings("TF2_VELOCITY", g_velocity_srv, g_velocity_srv);
		ensure_bb_views();
		tf2nr::force_reapply();
		logf("effect runtime %p attached", rt);
	}

	void on_destroy_effect_runtime(effect_runtime *rt)
	{
		std::lock_guard lock(g_mutex);
		if (rt != g_runtime) return;
		release_capture();
		release_hdr();
		release_bb_views();
		hl::release();
		g_runtime = nullptr;
		g_pending = false;
		logf("effect runtime %p detached", rt);
	}

	void on_reloaded_effects(effect_runtime *rt)
	{
		std::lock_guard lock(g_mutex);
		if (rt != g_runtime) return;
		tf2nr::force_reapply();
	}

	// Runs inside render_effects, after Generic Depth's own callback (built-in add-ons register first), so our
	// binding wins for the cut. Frames without a cut (menus) keep whatever DEPTH was bound last.
	void on_begin_effects(effect_runtime *rt, command_list *, resource_view, resource_view)
	{
		BR_SKIP_FG();
		std::lock_guard lock(g_mutex);
		if (rt != g_runtime || !g_cut_active || g_depth_srv.handle == 0) return;
		rt->update_texture_bindings("DEPTH", g_depth_srv, g_depth_srv);
	}

	void on_init_pipeline(device *dev, pipeline_layout, uint32_t count, const pipeline_subobject *subobjects, pipeline p)
	{
		if (subobjects == nullptr || foreign_dev(dev)) return;
		BR_SKIP_FG();
		for (uint32_t i = 0; i < count; ++i)
		{
			if (subobjects[i].type != pipeline_subobject_type::pixel_shader) continue;
			const auto *sd = static_cast<const shader_desc *>(subobjects[i].data);
			if (sd == nullptr || sd->code == nullptr || sd->code_size == 0) continue;
			// every engine_post_ps50 variant (film grain on/off, DoF, TSAA...) carries these RDEF names
			const bool match = (has_str(sd->code, sd->code_size, "CBufEnginePost") && has_str(sd->code, sd->code_size, "FBTexture")) ||
				fnv1a(sd->code, sd->code_size) == g_cfg_composite_ps;
			if (match)
			{
				std::lock_guard lock(g_mutex);
				g_composite_pipelines.insert(p.handle);
				logf("engine_post pixel shader %016llx created: pipeline %llx", (unsigned long long)fnv1a(sd->code, sd->code_size), (unsigned long long)p.handle);
			}
		}
	}

	void on_destroy_pipeline(device *dev, pipeline p)
	{
		if (foreign_dev(dev)) return;
		std::lock_guard lock(g_mutex);
		g_composite_pipelines.erase(p.handle);
	}

	void on_destroy_resource_view(device *dev, resource_view v)
	{
		if (foreign_dev(dev)) return;
		std::lock_guard lock(g_mutex);
		g_view_desc.erase(v.handle);
		g_view_res.erase(v.handle);
	}

	void on_destroy_resource(device *dev, resource r)
	{
		if (foreign_dev(dev)) return;
		std::lock_guard lock(g_mutex);
		if (r.handle == 0) return;
		if (r.handle == g_velocity.handle)
			release_velocity("game destroyed it");
		if (r.handle == g_hdr_fb.handle)
		{
			destroy_view(g_hdr_rtv); g_hdr_fb = { 0 };
			logf("HDR cut: FBTexture destroyed by the game");
		}
		if (r.handle == g_epost_cb) g_epost_cb = 0;
	}

	void on_bind_pipeline(command_list *cmd, pipeline_stage stages, pipeline p)
	{
		if ((stages & pipeline_stage::pixel_shader) == 0 || foreign_cmd(cmd)) return;
		BR_SKIP_FG();
		std::lock_guard lock(g_mutex);
		g_bound_ps = p.handle;
	}

	void on_push_descriptors(command_list *cmd, shader_stage stages, pipeline_layout, uint32_t, const descriptor_table_update &u)
	{
		if ((stages & shader_stage::pixel) == 0 || u.descriptors == nullptr || foreign_cmd(cmd)) return;
		BR_SKIP_FG();
		std::lock_guard lock(g_mutex);
		if (u.type == descriptor_type::shader_resource_view)
		{
			const auto *views = static_cast<const resource_view *>(u.descriptors);
			for (uint32_t i = 0; i < u.count; ++i)
				if (u.binding + i < 32) g_ps_srv[u.binding + i] = views[i];
		}
		else if (u.type == descriptor_type::constant_buffer && g_composite_pipelines.count(g_bound_ps) != 0 && u.binding <= 1 && u.binding + u.count > 1)
		{
			// learn CBufEnginePost (PS b1 of the engine_post draw)
			const buffer_range &r = static_cast<const buffer_range *>(u.descriptors)[1 - u.binding];
			if (r.buffer.handle == 0 || r.buffer.handle == g_epost_cb) return;
			const resource_desc d = cmd->get_device()->get_resource_desc(r.buffer);
			if (d.type == resource_type::buffer && d.buffer.size == 304)
			{
				g_epost_cb = r.buffer.handle;
				logf("CBufEnginePost = buffer %llx (304 bytes); KillGrain=%d", (unsigned long long)g_epost_cb, g_cfg_kill_grain);
			}
		}
	}

	void on_map_buffer(device *dev, resource res, uint64_t offset, uint64_t, map_access access, void **data)
	{
		if (res.handle == 0 || foreign_dev(dev)) return;
		BR_SKIP_FG();
		std::lock_guard lock(g_mutex);
		if (res.handle != g_epost_cb || data == nullptr || *data == nullptr || offset != 0) return;
		if (access != map_access::write_discard && access != map_access::write_only) return;
		g_epost_ptr = static_cast<uint8_t *>(*data);
		g_epost_mapped = res.handle;
	}

	void on_unmap_buffer(device *dev, resource res)
	{
		if (foreign_dev(dev)) return;
		BR_SKIP_FG();
		std::lock_guard lock(g_mutex);
		if (res.handle == 0 || res.handle != g_epost_mapped) return;
		uint8_t *p = g_epost_ptr;
		g_epost_ptr = nullptr; g_epost_mapped = 0;
		if (!g_cfg_kill_grain || !tf2nr::g_nr_enabled || p == nullptr) return;   // NR off in the menu = the normal game image
		const float zero2[2] = { 0.0f, 0.0f };
		memcpy(p + 160, zero2, sizeof(zero2));                   // c_noiseOffset (160), c_noiseScale (164); write-only
		++g_grain_patches;
	}

	void on_bind_rts(command_list *cmd, uint32_t count, const resource_view *rtvs, resource_view dsv)
	{
		if (foreign_cmd(cmd) || (count > 0 && rtvs == nullptr)) return;
		BR_SKIP_FG();
		std::lock_guard lock(g_mutex);
		if (g_in_render) return;
		check_pending(cmd, "bind_render_targets");
		device *dev = cmd->get_device();
		g_bound_rt0_res = count >= 1 ? res_of_view(dev, rtvs[0]) : 0;
		g_bound_rtv0_view = count >= 1 ? rtvs[0] : resource_view { 0 };
		g_bound_dsv_res = res_of_view(dev, dsv);
		g_bound_dsv_view = dsv;
		if (!g_cfg_mv || count < 2 || rtvs[0].handle == 0 || rtvs[1].handle == 0) return;
		uint64_t r0 = 0, r1 = 0;
		const resource_desc *d0 = desc_of_view(dev, rtvs[0], &r0);
		const resource_desc *d1 = desc_of_view(dev, rtvs[1], &r1);
		if (!d0 || !d1 || d0->type != resource_type::texture_2d || d1->type != resource_type::texture_2d) return;
		if (!is_rgba16(d0->texture.format) || !is_rg16(d1->texture.format)) return;
		if (d0->texture.width != d1->texture.width || d0->texture.height != d1->texture.height) return;
		++g_main_binds;
		if (r1 != g_velocity.handle)
			adopt_velocity(dev, r1, *d1);
	}

	void on_any_draw(command_list *cmd, const char *where)
	{
		if (foreign_cmd(cmd)) return;
		BR_SKIP_FG();
		std::lock_guard lock(g_mutex);
		if (g_in_render) return;
		check_pending(cmd, where);
		// this draw executes after we return: is it engine_post into the back buffer?
		if (g_runtime == nullptr || g_composite_pipelines.count(g_bound_ps) == 0 ||
			g_bound_rt0_res == 0 || g_bound_rt0_res != g_runtime->get_current_back_buffer().handle)
			return;
		// 0.4.1: this draw is the HUD-less capture point. The previous frame's copy stops being VALID here; hudless_capture
		// makes this frame's valid again (a cut that does not run or copies nothing leaves it invalid, as the header says).
		{
			std::lock_guard lk(hl::mx);
			hl::s.valid = false;
		}
		if (!g_cfg_prehud) return;
		++g_composites;
		// NR off in the Video menu: no cut, nothing to feed - unless FG wants the HUD-less copy. Then the LDR cut still runs
		// (effects before the HUD, never at present on top of it: FSR would take the effect delta for UI) and copies.
		g_cut_for_fg = !tf2nr::g_nr_enabled && hudless_wanted();
		if (!tf2nr::g_nr_enabled && !g_cut_for_fg) return;
		if (tf2nr::g_nr_enabled && g_cfg_hdrcut && !g_hdr_ran_this_frame && hdr_bracket_enabled() && cmd == g_runtime->get_command_queue()->get_immediate_command_list() && ensure_bb_views())
			run_hdr_cut(cmd);
		g_pending = true;                                        // LDR cut (or the HDR capture's 'fin') after the draw
	}

	bool on_draw(command_list *cmd, uint32_t, uint32_t, uint32_t, uint32_t) { on_any_draw(cmd, "draw"); return false; }
	bool on_draw_indexed(command_list *cmd, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { on_any_draw(cmd, "draw_indexed"); return false; }
	bool on_draw_indirect(command_list *cmd, indirect_command, resource, uint64_t, uint32_t, uint32_t) { on_any_draw(cmd, "draw_indirect"); return false; }

	bool on_clear_ds(command_list *cmd, resource_view, const float *, const uint8_t *, uint32_t, const rect *)
	{
		if (foreign_cmd(cmd)) return false;
		BR_SKIP_FG(false);
		std::lock_guard lock(g_mutex);
		if (!g_in_render) check_pending(cmd, "clear_depth_stencil");
		return false;
	}

	bool on_clear_rt(command_list *cmd, resource_view, const float[4], uint32_t, const rect *)
	{
		if (foreign_cmd(cmd)) return false;
		BR_SKIP_FG(false);
		std::lock_guard lock(g_mutex);
		if (!g_in_render) check_pending(cmd, "clear_render_target");
		return false;
	}

	bool key_edge(int vk, bool &was_down)
	{
		if (vk <= 0) return false;
		const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
		const bool edge = down && !was_down;
		was_down = down;
		if (!edge) return false;
		DWORD pid = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &pid);
		return pid == GetCurrentProcessId();
	}

	// tf2-fg link, once per frame (GetModuleHandleW; GetProcAddress only when the module changes).
	void refresh_fg_link()
	{
		HMODULE m = GetModuleHandleW(L"tf2-fg.addon64");
		if (m == g_fg_mod) return;
		g_fg_mod = m;
		const auto f = m ? reinterpret_cast<PFN_tf2fg_in_native_present>(GetProcAddress(m, "tf2fg_in_native_present")) : nullptr;
		g_fg_native.store(f);
		logf("tf2-fg link: %s", m == nullptr ? "tf2-fg.addon64 not loaded" : (f ? "tf2fg_in_native_present resolved - per-draw work is skipped while tf2-fg's native Present runs" : "tf2-fg.addon64 has no tf2fg_in_native_present export"));
	}

	void on_present(command_queue *queue, swapchain *, const rect *, const rect *, uint32_t, const rect *)
	{
		if (queue == nullptr) return;
		{
			device *const qd = queue->get_device();
			if (qd != nullptr && g_game_dev.load(std::memory_order_relaxed) == nullptr && qd->get_api() == device_api::d3d11)
			{
				g_game_dev.store(qd);
				logf("game device %p (D3D11, first present) - events of any other device are ignored", static_cast<void *>(qd));
			}
			if (foreign_dev(qd)) return;
		}
		// 0.4.1: like tf2-dlss-sr's present callback: a present raised inside tf2-fg's own native Present is not a game frame
		BR_SKIP_FG();
		std::lock_guard lock(g_mutex);
		refresh_fg_link();
		// HUD-less export: a frame presented without its own copy invalidates the snapshot.
		{
			std::lock_guard lk(hl::mx);
			if (hl::s.valid && hl::s.frame == g_frame) ++hl::win_valid_frames;
			else hl::s.valid = false;
		}
		g_pending = false;   // engine_post without a later event this frame: present-time run handles it
		g_hdr_ran_this_frame = false;
		g_cut_for_fg = false;
		++g_frame; ++g_window_frames;
		hl::frame_now.store(g_frame);
		tf2nr::on_present(g_runtime, &logf);
		if (key_edge(g_cfg_toggle_key, g_key_toggle_down))
		{
			g_cfg_prehud = !g_cfg_prehud;
			logf("PreHUD toggled %s (hotkey)", g_cfg_prehud ? "ON" : "OFF");
		}
		if (key_edge(g_cfg_hdr_key, g_key_hdr_down))
		{
			g_cfg_hdrcut = !g_cfg_hdrcut;
			logf("HdrCut toggled %s (hotkey)", g_cfg_hdrcut ? "ON" : "OFF");
		}
		if (key_edge(g_cfg_dump_key, g_key_dump_down))
		{
			g_dump_left = std::max(1, g_cfg_dump_burst);
			g_dump_wait = 0;
			logf("dump: burst of %d armed (every %d frames)", g_dump_left, g_cfg_dump_interval);
		}
		if (g_dump_left > 0 && --g_dump_wait <= 0)
		{
			--g_dump_left;
			g_dump_wait = std::max(1, g_cfg_dump_interval);
			dump_velocity(queue->get_immediate_command_list());
		}
		if (key_edge(g_cfg_capture_key, g_key_capture_down) && g_cap_next < 0)
			arm_capture();
		if (g_cap_next >= 0 && g_cap_next >= static_cast<int>(g_cap.size()))
			write_capture();
		if (g_window_frames >= 600)
		{
			logf("frame %llu: velocity %s, main binds %llu, engine_post %llu, LDR cuts %llu, HDR cuts %llu /%llu frames (skipped %llu), grain patches %llu, PreHUD=%d HdrCut=%d NR=%d SR=%d",
				(unsigned long long)g_frame, g_velocity_srv.handle ? "bound" : "NOT bound", (unsigned long long)g_main_binds,
				(unsigned long long)g_composites, (unsigned long long)g_window_prehud, (unsigned long long)g_window_hdr, (unsigned long long)g_window_frames,
				(unsigned long long)g_skipped, (unsigned long long)g_grain_patches, g_cfg_prehud, g_cfg_hdrcut, (int)tf2nr::g_nr_enabled, (int)tf2nr::g_sr_mode);
			logf("   hudless: copies %llu (LDR cut %llu, HDR %llu, cut run for FG with NR off %llu, failed %llu) | valid presented frames %llu/%llu | export calls %llu (valid %llu, refused %llu) | "
				"demand %s (HudlessCopy=%d) | generation %llu serial %llu | tf2-fg link %s, events skipped in its native Present %llu | foreign-device events ignored %llu",
				(unsigned long long)hl::win_captures, (unsigned long long)hl::win_ldr, (unsigned long long)hl::win_hdr, (unsigned long long)hl::win_fgcut,
				(unsigned long long)hl::win_fail, (unsigned long long)hl::win_valid_frames, (unsigned long long)g_window_frames,
				(unsigned long long)hl::calls.exchange(0), (unsigned long long)hl::calls_valid.exchange(0), (unsigned long long)hl::calls_bad.exchange(0),
				hudless_wanted() ? "yes" : "no", g_cfg_hudless, (unsigned long long)hl::generation, (unsigned long long)hl::serial,
				g_fg_native.load() ? "yes" : "no", (unsigned long long)g_fg_skips.exchange(0), (unsigned long long)g_foreign_events.exchange(0));
			hl::win_captures = hl::win_ldr = hl::win_hdr = hl::win_fgcut = hl::win_fail = hl::win_valid_frames = 0;
			g_window_frames = 0; g_window_prehud = 0; g_window_hdr = 0; g_composites = 0; g_main_binds = 0; g_grain_patches = 0;
		}
	}

	void on_destroy_device(device *dev)
	{
		std::lock_guard lock(g_mutex);
		if (dev != g_device) return;
		release_velocity("device destroyed");
		release_capture();
		release_hdr();
		release_bb_views();
		hl::release();
		g_fg_native.store(nullptr); g_fg_mod = nullptr;   // re-resolved on the next present (add-ons may be reloaded)
		g_view_desc.clear();
		g_view_res.clear();
		g_device = nullptr;
		g_game_dev.store(nullptr);   // learned again at the next device's first present
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// FG export (tf2_fg_interop.h). Any thread; takes only hl::mx; no D3D calls except AddRef; never throws.
// Returns 1 = VALID (texture AddRef'd for the caller), 0 = not valid (no reference), -1 = bad argument (null or
// struct_size < sizeof(tf2nr_hudless_v1); nothing written), -2 = internal exception. Each call is also the demand signal.
extern "C" __declspec(dllexport) int tf2nr_get_hudless(tf2nr_hudless_v1 *io)
{
	if (io == nullptr || io->struct_size < sizeof(tf2nr_hudless_v1)) { hl::calls_bad.fetch_add(1); return -1; }
	try
	{
		hl::demand_frame.store(hl::frame_now.load());
		hl::demanded.store(true);
		tf2nr_hudless_v1 o = {};
		o.struct_size = sizeof(o);
		o.version = TF2NR_HUDLESS_VERSION;
		o.dxgi_format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
		int ret = 0;
		{
			std::lock_guard lk(hl::mx);
			const hl::snap_t &s = hl::s;
			const uint64_t now = hl::frame_now.load();
			o.exporter_frame = now;
			o.game_frame = s.frame;
			o.capture_serial = s.serial;
			o.generation = s.generation;
			o.ring_index = static_cast<uint32_t>(s.ring);
			o.flags = s.flags & ~TF2NR_HUDLESS_VALID;
			o.back_buffer_format = s.bb_fmt;
			o.width = s.w; o.height = s.h;
			ID3D11Texture2D *t = (s.ring == 0 || s.ring == 1) ? hl::tex[s.ring] : nullptr;
			const bool valid = s.valid && t != nullptr && now >= s.frame && now - s.frame <= 1 && s.generation == hl::generation;
			if (valid)
			{
				o.flags |= TF2NR_HUDLESS_VALID;
				o.texture = t; t->AddRef();
				ret = 1;
			}
		}
		*io = o;
		hl::calls.fetch_add(1);
		if (ret == 1) hl::calls_valid.fetch_add(1);
		return ret;
	}
	catch (...)
	{
		return -2;
	}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
	{
		if (!reshade::register_addon(module))
			return FALSE;
		wchar_t path[MAX_PATH] = {};
		GetModuleFileNameW(module, path, MAX_PATH);
		g_dir = std::filesystem::path(path).parent_path();
		g_log = _wfopen((g_dir / "tf2-nr-bridge.log").c_str(), L"w");
		logf("tf2-nr-bridge 0.4.1 loaded (engine velocity, LDR/HDR cut, KillGrain, Video-menu toggles, FG export tf2nr_get_hudless v%u, tf2-fg native-present skip, game-device filter)", TF2NR_HUDLESS_VERSION);
		load_config();
		reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
		reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
		reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
		reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
		reshade::register_event<reshade::addon_event::destroy_resource_view>(on_destroy_resource_view);
		reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
		reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
		reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
		reshade::register_event<reshade::addon_event::map_buffer_region>(on_map_buffer);
		reshade::register_event<reshade::addon_event::unmap_buffer_region>(on_unmap_buffer);
		reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_rts);
		reshade::register_event<reshade::addon_event::draw>(on_draw);
		reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_draw_indirect);
		reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(on_clear_ds);
		reshade::register_event<reshade::addon_event::clear_render_target_view>(on_clear_rt);
		reshade::register_event<reshade::addon_event::present>(on_present);
		reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
		break;
	}
	case DLL_PROCESS_DETACH:
		reshade::unregister_addon(module);
		if (g_log) { fclose(g_log); g_log = nullptr; }
		break;
	}
	return TRUE;
}
