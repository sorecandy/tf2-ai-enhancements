// tf2_dlss_sr.cpp - tf2-dlss-sr 0.3: NVIDIA DLSS Super Resolution in place of Titanfall 2's TSAA resolve.
// ReShade 6.8.0 add-on (API 20), D3D11 immediate context, NGX SDK 310.x (static nvsdk_ngx_s.lib).
//
//   TF2 renders the scene at its viewport scale into the top-left sub-rect of its full-size RTs, with a Halton
//   projection jitter while TSAA (mat_antialias_mode 12) is on. The scale comes from client.dll's DVS controller
//   (RVA 0x358dc0): dvs_enable 0 -> it returns mat_viewportscale as-is (instant, no ramp); dvs_enable 1 -> it ramps
//   the scale a few % per step towards dvs_scale_min. 0.2 pins the DLSS scale through mat_viewportscale (PinMethod=1).
//   The render size used for DLSS is the one the TSAA pass itself uses (CBufTSAA c_fbMaxUVs, 0.3), cross-checked
//   against this frame's dominant main-pass viewport.
//   At the TSAA draw (PS RDEF contains "CBufTSAA" + "VelocityBuffer") this add-on
//     1. copies the render sub-rect of the TSAA inputs into typed textures (prep_cs: colour+alpha, jitter-free
//        motion vectors in render pixels, depth as R32F),
//     2. evaluates DLSS SR (render -> display size, HDR, engine MVs, TF2's jitter, TF2's exposure, alpha upscaling
//        so engine_post still sees the cloak mask) into its own UAV texture,
//     3. lets the TSAA draw run (it also writes TF2's other MRTs) and overwrites the TSAA colour target with the
//        DLSS result right before the next GPU command.
//   Probe=1 logs the TSAA pass state (P0: bindings, viewport, CBufTSAA jitter range, context type) whether or not
//   DLSS runs. Evaluate=0 makes the add-on a pure probe.
//
// Config: [TF2DLSSSR] in ReShade.ini
//   Evaluate=0|1        run DLSS (0 = probe only)
//   ForceMode=-1        -1 = follow the Video-menu convar tf2nr_dlss_sr (0 off,1 DLAA,2 Q,3 B,4 P,5 UP); 0..5 = fixed
//   JitterSignX/Y=-1|1  DLSS jitter = sign * c_jitter * render size
//   MVJittered=1|0      1 (default) = TF2 velocity = true motion - jitter (TSAA: prev = uv - (v/1024 + c_jitter)): add the jitter back in prep
//   DepthInverted=0|1   Preset=11 (K)   Probe=1   DumpShaders=1
//   SignKey=0           VK code: cycle the 4 jitter-sign combinations (logged)
//   MVKey=0             VK code: toggle MVJittered at runtime (logged)
//   PinMethod=1         1 = dvs_enable 0 + mat_viewportscale S (instant); 0 = legacy DVS pin (dvs 1, gpuframetime 0/1,
//                       dvs_scale_min S: ramps over seconds). 1 falls back to 0 if mat_viewportscale cannot be resolved.
//   PresetPerMode=1     1 = per-mode presets below (NVIDIA defaults K/K/K/M/L); 0 = the single Preset for every mode
//   PresetDLAA=11 PresetQuality=11 PresetBalanced=11 PresetPerformance=13 PresetUltraPerformance=12
//   SettleFrames=3      a new render size must be seen this many consecutive frames before inputs/feature follow it
//   RecreateCooldown=45 frames between input reallocations / feature re-creations (renodx rebuilds NR on each)
//   MipBias=1           negative texture LOD bias in the main scene pass for upscaled modes (Q/B/P/UP, not DLAA)
//   MipBiasOffset=-1.0  bias = log2(render width / display width) + MipBiasOffset, clamped to [-3, 0]
//   SizeSource=0        render size: 0 auto (TSAA's own sub-rect from CBufTSAA c_fbMaxUVs, cross-checked with this
//                       frame's dominant main-pass viewport; TSAA's wins on disagreement), 1 main-pass viewport only,
//                       2 b2 CPU shadow only (legacy, never valid in practice), 3 TSAA c_fbMaxUVs only
//   CameraReadback=1    decode the main view's camera (near/far/fovY) for the FG export; 0 = off
//   NoTsaaUnpin=30      frames in a row with a main scene pass but no TSAA pass (while an upscaling mode pins
//                       mat_viewportscale < 1) after which the pin is lifted until a TSAA pass is seen again; 0 = never
//   CamCutDist=64       FG camera-cut hint: main-view camera moved more than this many world units (inches) in one frame
//   CamCutDeg=40        ... or its view direction turned more than this many degrees in one frame
//   CamCutFovDeg=15     ... or its vertical field of view changed more than this many degrees in one frame
//
// 0.3.0 (2026-09-29, M3 sr_bridge track):
//   - death-cam fix: on some deaths (death cam -> spectator transition) the LAST viewport TF2 binds while the main scene
//     MRT is bound is the full 1920x1200, but no draw uses it: the scene is still drawn into the DVS sub-rect and TSAA
//     still upscales that sub-rect (c_fbMaxUVs keep 0.58). 0.2 took the render size from that last viewport (its b2
//     cross-check never ran: ReShade reports CBufCommonPerCamera's whole-buffer UpdateSubresource with size UINT64_MAX,
//     which the 0.2 shadow dropped), so DLSS got a 1920x1200 "render" rect = the scene in the top-left corner + stale RT
//     around it, output 1:1. Verified in game 2026-09-29 (UP and Balanced): 29-55 frames per affected death where 0.2's
//     rule gives 1920x1200 while TSAA's clamp and the draw-weighted main-pass viewport give the sub-rect; the HUD-less
//     frames of those deaths are full-screen with 0.3. 0.3 takes the size TSAA itself uses (c_fbMaxUVs), confirms it
//     with the draw-weighted dominant main-pass viewport (TSAA wins and is logged on disagreement) and logs the frames
//     where 0.2's rule would have differed;
//   - no-TSAA guard (NoTsaaUnpin): while an upscaling mode is pinned, frames that draw the scene but have no TSAA pass lift
//     the pin until TSAA returns (nothing would replace TSAA; the engine's own bilinear path is all that runs);
//   - FG export tf2sr_get_fg_inputs (bridge\tf2_fg_interop.h): AddRef'd depth / MVs / exposure, jitter, sizes, reset,
//     camera near/far/fov of the main view (CBufCommonPerCamera upload at the first main-pass draw, CameraReadback=1),
//     per-frame validity + generation;
//   - every per-draw callback returns at once while tf2-fg's own native Present runs (tf2fg_in_native_present()).
//
// 0.3.1 (2026-09-29, M3 review fixes):
//   - only the game's D3D11 device is served: renodx-dlss5 creates a private D3D12 device through ReShade's hooked
//     D3D12CreateDevice, so its command lists/resources could raise events here. Every callback now returns at once for
//     another device (the game's device = the one whose present event arrives first, D3D11 only before that), and
//     destroy_device tears SR down only for the game's device (a renodx D3D12 teardown used to shut NGX down);
//   - CBufCommonPerCamera (b2): forgotten when the game destroys that buffer, the whole-buffer upload is copied only when
//     the destination really is 576 bytes, and the handle is compared (atomic) before any lock;
//   - TSAA's b2 constants (c_forceExposure, c_renderTargetSize, c_framebufferViewportScale) now come from the latest b2
//     upload at the TSAA draw (the path the old shadow never saw), so the forced-exposure path is live again;
//   - Probe=1 detail dumps really stop after 8 windows and only read the TSAA inputs (t0 t1 t4 t5 t30), which the game
//     binds for that draw; slots of destroyed resources are cleared;
//   - camera-cut hint for FG (TF2SR_FG_CAMERA_CUT): the main view's camera position / view direction / fovY jumped
//     between consecutive frames by more than CamCutDist / CamCutDeg / CamCutFovDeg (per-window margins logged).
// 0.3.2 (2026-09-29, ai:sr) = 0.3.1 + the 0.4.1 fixes WITHOUT the FSR 3 backend: the renodx native route (and 'NR on +
//   tf2nr_dlss_sr 0 -> DLSS DLAA') needs renodx-dlss5.addon64 loaded (not in [ADDON] DisabledAddons) besides DX11Source=native;
//   an unread tf2nr_dlss5_nr never counts as NR on; nrsync never presses F6 without renodx; NGX DLL search = add-on folder,
//   then the game exe folder. tf2nr_upscaler is not read (DLSS always; logged once at load).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <cstdint>
#include <cstdio>
#include <share.h>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <atomic>
#include <mutex>
#include <string>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <reshade.hpp>
#include "nvsdk_ngx_helpers.h"
#include "prep_cs.h"   // const BYTE g_prep_cs[] (fxc /Vn g_prep_cs)
#include "../bridge/tf2nr_menu_toggles.hpp"
#include "tf2_nr_linux_api.h"   // tf2-nr-linux: DLSS 5 NR under Linux/Proton (separate GPL add-on, github.com/Milofax/tf2-nr-linux; header MIT)
#include "../bridge/tf2nr_nr_tuning.hpp"   // live DLSS 5 Adjustments sliders -> renodx atomics
#include "../bridge/tf2_fg_interop.h"     // tf2sr_fg_inputs_v1 (FG export), PFN_tf2fg_in_native_present

extern "C" __declspec(dllexport) const char *NAME = "TF2 DLSS SR";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Titanfall 2: DLSS Super Resolution in place of TSAA (engine jitter, MVs, depth, exposure).";

using namespace reshade::api;

namespace
{
	HMODULE g_module = nullptr;
	std::filesystem::path g_dir;
	FILE *g_log = nullptr;
	std::mutex g_mx;
	// Re-entrancy: renodx hooks NGX and, inside our DLSS evaluate, issues commands through ReShade, which fire these
	// callbacks again on the same thread. Locking g_mx twice there throws (MSVC std::mutex detects it) -> terminate
	// (2026-09-28 P1 crash, ucrtbase fail-fast 7). Nested events are the add-ons' own commands: skip them.
	thread_local int t_depth = 0;
	struct depth_guard { depth_guard() { ++t_depth; } ~depth_guard() { --t_depth; } };
	uint64_t g_nested = 0;   // nested events skipped (diagnostic, logged per 600 frames)

	// tf2-fg link: while tf2-fg's own native IDXGISwapChain::Present runs on this thread, any event we get comes from a
	// third-party present hook rendering through ReShade's proxy in the middle of a game frame -> ignore it. Resolved once
	// per frame in on_present (tf2-fg loads after us: add-ons load alphabetically), dropped with the device.
	std::atomic<PFN_tf2fg_in_native_present> g_fg_native { nullptr };
	HMODULE g_fg_mod = nullptr;
	std::atomic<uint64_t> g_fg_skips { 0 };
	inline bool fg_native()
	{
		const PFN_tf2fg_in_native_present f = g_fg_native.load(std::memory_order_relaxed);
		return f != nullptr && f();
	}
	// 0.3.1: only the game's D3D11 device is served. renodx-dlss5 creates its own D3D12 device through ReShade's hooked
	// D3D12CreateDevice (ReShade.log 'Redirecting D3D12CreateDevice'), so events of its command lists / resources can reach
	// every add-on; ours reinterpret get_native() as ID3D11DeviceContext and would call past a D3D12 vtable. The game's
	// device is the one whose present event arrives first (learned in on_present); before that, D3D11 only.
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
	// Per-draw/bind/present callbacks. Destroy callbacks use SR_LOCK_ALWAYS: a skipped destroy would leave a stale cache.
#define SR_LOCK(...) if (t_depth) { ++g_nested; return __VA_ARGS__; } if (fg_native()) { g_fg_skips.fetch_add(1, std::memory_order_relaxed); return __VA_ARGS__; } depth_guard sr_depth_guard_; std::lock_guard lock(g_mx)
#define SR_LOCK_ALWAYS(...) if (t_depth) { ++g_nested; return __VA_ARGS__; } depth_guard sr_depth_guard_; std::lock_guard lock(g_mx)

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

	// ------------------------------------------------------------------ config
	struct cfg_t
	{
		int evaluate = 0, force_mode = -1, mv_jittered = 1, depth_inverted = 0, preset = 11, probe = 1, dump_shaders = 1;
		int sign_key = 0, mv_key = 0;
		float jsx = -1.0f, jsy = -1.0f;
		int pin_method = 1, preset_per_mode = 1;
		int preset_mode[6] = { 0, 11, 11, 11, 13, 12 };   // index = SR mode (1 DLAA .. 5 UP); K K K M L
		int settle_frames = 3, recreate_cooldown = 45, mip_bias = 1, size_source = 0;
		float mip_bias_offset = -1.0f;
		int no_tsaa_unpin = 30, camera_readback = 1;
		int nr_order = 0;   // tf2-nr-linux: 0 = after DLSS when render size == display size (DLAA), else before; 1 = always before
		float cam_cut_dist = 64.0f, cam_cut_deg = 40.0f, cam_cut_fov_deg = 15.0f;
	} g_cfg;

	int preset_for_mode(int mode)
	{
		if (!g_cfg.preset_per_mode || mode < 1 || mode > 5) return g_cfg.preset;
		return g_cfg.preset_mode[mode];
	}

	void load_config()
	{
		reshade::get_config_value(nullptr, "TF2DLSSSR", "Evaluate", g_cfg.evaluate);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "ForceMode", g_cfg.force_mode);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "MVJittered", g_cfg.mv_jittered);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "DepthInverted", g_cfg.depth_inverted);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "Preset", g_cfg.preset);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "Probe", g_cfg.probe);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "DumpShaders", g_cfg.dump_shaders);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "SignKey", g_cfg.sign_key);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "MVKey", g_cfg.mv_key);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "JitterSignX", g_cfg.jsx);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "JitterSignY", g_cfg.jsy);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "PinMethod", g_cfg.pin_method);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "PresetPerMode", g_cfg.preset_per_mode);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "PresetDLAA", g_cfg.preset_mode[1]);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "PresetQuality", g_cfg.preset_mode[2]);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "PresetBalanced", g_cfg.preset_mode[3]);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "PresetPerformance", g_cfg.preset_mode[4]);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "PresetUltraPerformance", g_cfg.preset_mode[5]);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "SettleFrames", g_cfg.settle_frames);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "RecreateCooldown", g_cfg.recreate_cooldown);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "MipBias", g_cfg.mip_bias);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "MipBiasOffset", g_cfg.mip_bias_offset);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "SizeSource", g_cfg.size_source);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "NoTsaaUnpin", g_cfg.no_tsaa_unpin);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "CameraReadback", g_cfg.camera_readback);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "CamCutDist", g_cfg.cam_cut_dist);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "CamCutDeg", g_cfg.cam_cut_deg);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "CamCutFovDeg", g_cfg.cam_cut_fov_deg);
		reshade::get_config_value(nullptr, "TF2DLSSSR", "NROrder", g_cfg.nr_order);
		const auto clampf = [](float v, float lo, float hi) { return !(v >= lo) ? lo : (v > hi ? hi : v); };
		g_cfg.cam_cut_dist = clampf(g_cfg.cam_cut_dist, 1.0f, 1.0e6f);
		g_cfg.cam_cut_deg = clampf(g_cfg.cam_cut_deg, 1.0f, 180.0f);
		g_cfg.cam_cut_fov_deg = clampf(g_cfg.cam_cut_fov_deg, 0.5f, 180.0f);
		g_cfg.no_tsaa_unpin = g_cfg.no_tsaa_unpin < 0 ? 0 : (g_cfg.no_tsaa_unpin > 100000 ? 100000 : g_cfg.no_tsaa_unpin);
		g_cfg.settle_frames = g_cfg.settle_frames < 1 ? 1 : (g_cfg.settle_frames > 120 ? 120 : g_cfg.settle_frames);
		g_cfg.recreate_cooldown = g_cfg.recreate_cooldown < 0 ? 0 : (g_cfg.recreate_cooldown > 600 ? 600 : g_cfg.recreate_cooldown);
		logf("config: Evaluate=%d ForceMode=%d JitterSign=%+.0f,%+.0f MVJittered=%d DepthInverted=%d Preset=%d Probe=%d DumpShaders=%d SignKey=%d MVKey=%d",
			g_cfg.evaluate, g_cfg.force_mode, g_cfg.jsx, g_cfg.jsy, g_cfg.mv_jittered, g_cfg.depth_inverted, g_cfg.preset, g_cfg.probe,
			g_cfg.dump_shaders, g_cfg.sign_key, g_cfg.mv_key);
		logf("config: PinMethod=%d PresetPerMode=%d (DLAA %d, Q %d, B %d, P %d, UP %d) SettleFrames=%d RecreateCooldown=%d MipBias=%d MipBiasOffset=%.2f SizeSource=%d NoTsaaUnpin=%d CameraReadback=%d",
			g_cfg.pin_method, g_cfg.preset_per_mode, preset_for_mode(1), preset_for_mode(2), preset_for_mode(3), preset_for_mode(4), preset_for_mode(5),
			g_cfg.settle_frames, g_cfg.recreate_cooldown, g_cfg.mip_bias, g_cfg.mip_bias_offset, g_cfg.size_source, g_cfg.no_tsaa_unpin, g_cfg.camera_readback);
		logf("config: FG camera-cut hint: CamCutDist=%.1f units CamCutDeg=%.1f CamCutFovDeg=%.1f (per frame, main view)", g_cfg.cam_cut_dist, g_cfg.cam_cut_deg, g_cfg.cam_cut_fov_deg);
	}

	// renodx DX11Source=native: NR runs inside this add-on's DLSS evaluate, so with NR on the SR row's "Off" still
	// runs DLSS as DLAA (native resolution, no upscaling) - otherwise NR would silently stop with SR.
	// 0.4.1: the native route (and the "NR on + Upscaler Off -> DLSS Native AA" rule) also needs renodx-dlss5.addon64 to be
	// LOADED in this process and not listed in [ADDON] DisabledAddons: the package's ReShade.ini carries DX11Source=native
	// for users who add renodx later, and without renodx tf2nr_dlss_sr 0 must mean no upscaler at all. Checked on every
	// refresh (load, then every 300 frames): renodx may load after this add-on.
	bool g_native_route = false;
	int g_route_logged = -1;
	bool renodx_active(bool *loaded_out)
	{
		const bool loaded = GetModuleHandleW(L"renodx-dlss5.addon64") != nullptr;
		if (loaded_out) *loaded_out = loaded;
		if (!loaded) return false;
		char dis[1024] = {}; size_t dl = sizeof(dis) - 1;
		if (reshade::get_config_value(nullptr, "ADDON", "DisabledAddons", dis, &dl))
		{
			for (size_t i = 0; i + 1 < sizeof(dis) && i < dl; ++i) if (dis[i] == 0) dis[i] = ',';   // list elements may come NUL-separated
			dis[sizeof(dis) - 1] = 0;
			if (strstr(dis, "DLSS 5 Neural Rendering") != nullptr || strstr(dis, "renodx-dlss5") != nullptr) return false;
		}
		return true;
	}
	// tf2-nr-linux route: tf2-nr-linux.addon64 runs DLSS 5 NR with DynamicShaderFrameGen's runtime, which
	// works under Proton where renodx-dlss5 does not. It is called from run_dlss (no NGX hook): after the DLSS evaluate
	// on the DLSS output when render size == display size (DLAA - the same place as in Skyrim), otherwise before it on
	// the render-size colour (renodx's NRPreUpscale placement). Same menu rules as the renodx native route.
	PFN_tf2nr_linux_apply g_nrl_apply = nullptr;
	PFN_tf2nr_linux_reset g_nrl_reset = nullptr;
	bool g_nrl_route = false;
	int g_nrl_logged = -1;
	uint64_t g_nrl_ok = 0, g_nrl_fail = 0;
	void refresh_nrl()
	{
		HMODULE m = GetModuleHandleW(L"tf2-nr-linux.addon64");
		g_nrl_apply = m ? reinterpret_cast<PFN_tf2nr_linux_apply>(GetProcAddress(m, "tf2nr_linux_apply")) : nullptr;
		g_nrl_reset = m ? reinterpret_cast<PFN_tf2nr_linux_reset>(GetProcAddress(m, "tf2nr_linux_reset")) : nullptr;
		g_nrl_route = g_nrl_apply != nullptr;
		const int code = (m ? 1 : 0) | (g_nrl_route ? 2 : 0);
		if (code != g_nrl_logged)
		{
			g_nrl_logged = code;
			logf("tf2-nr-linux route: %s", g_nrl_route ? "active (DLSS 5 NR from tf2-nr-linux.addon64; NR on keeps DLSS at least at Native AA)" :
			                                             (m ? "add-on loaded but its exports are missing" : "none"));
		}
	}
	void refresh_route()
	{
		refresh_nrl();
		char buf[32] = {}; size_t len = sizeof(buf);
		const bool ini_native = reshade::get_config_value(nullptr, "RenoDX.DLSS5", "DX11Source", buf, &len) && _stricmp(buf, "native") == 0;
		bool loaded = false;
		const bool active = ini_native && renodx_active(&loaded);
		g_native_route = active;
		const int code = (ini_native ? 1 : 0) | (loaded ? 2 : 0) | (active ? 4 : 0);
		if (code != g_route_logged)
		{
			g_route_logged = code;
			logf("renodx route: %s (DX11Source=native: %s, renodx-dlss5.addon64 loaded: %s%s)",
				active ? "native (NR inside this DLSS evaluate; NR on keeps DLSS at least at Native AA)" : "none (DLSS 5 NR not available here; Upscaler Off = no upscaler)",
				ini_native ? "yes" : "no", loaded ? "yes" : "no", (loaded && ini_native && !active) ? ", but listed in [ADDON] DisabledAddons" : "");
		}
	}
	// NR as the menu says it - an UNREAD tf2nr_dlss5_nr (the bridge header defaults g_nr_enabled to 1) never counts as on
	bool nr_menu_on() { return tf2nr::g_nr_read != 0 && tf2nr::g_nr_enabled != 0; }

	int current_mode()
	{
		int m = g_cfg.force_mode >= 0 ? g_cfg.force_mode : tf2nr::g_sr_mode;
		m = m < 0 ? 0 : (m > 5 ? 5 : m);
		if (m == 0 && (g_native_route || g_nrl_route) && nr_menu_on()) m = 1;
		return m;
	}

	// The render size the engine produces for a mode (client.dll 0x35bb60: h = round(H * scale), w = round(h * W/H)).
	// 1920x1200: Q 1280x800, B 1114x696, P 960x600, UP 640x400.
	void mode_target(int mode, uint32_t ow, uint32_t oh, uint32_t &tw, uint32_t &th)
	{
		const float s = tf2nr::sr_render_scale(static_cast<tf2nr::sr_mode>(mode));
		th = static_cast<uint32_t>(std::lround(oh * s));
		tw = static_cast<uint32_t>(std::lround(th * (static_cast<float>(ow) / static_cast<float>(oh ? oh : 1))));
		if (th == 0) th = 1;
		if (tw == 0) tw = 1;
		if (tw > ow) tw = ow;
		if (th > oh) th = oh;
	}

	// ------------------------------------------------------------------ engine settings SR depends on
	// DLSS SR needs TSAA (its pass is what we replace; it also turns on TF2's jitter) and Adaptive Resolution pinned
	// at the DLSS render scale. Script cannot touch these convars ("ConVar mat_antialias_mode is not valid" in the
	// UI VM), so the add-on writes their LIVE numeric values (ConVar +0x58 float, +0x5C int on the parent) while SR
	// is on and puts the originals back when it goes off. The string value is left alone, so videoconfig.txt keeps
	// the player's own settings.
	namespace eng
	{
		struct cv { const char *name; void *ptr = nullptr; bool have_orig = false; float f0 = 0; int i0 = 0; };
		// mat_antialias_mode is NOT a registered convar (TF2's video-config system owns it; FindVar returns null),
		// so TSAA itself must be chosen in Settings > Video > Anti-aliasing. Only Adaptive Resolution is driven here.
		cv dvs { "dvs_enable" }, gmin { "dvs_gpuframetime_min" }, gmax { "dvs_gpuframetime_max" }, smin { "dvs_scale_min" };
		// mat_viewportscale (materialsystem_dx11.dll ConVar @RVA 0x1a9e810, default "1.0", range 0.0015625..2.0, flags
		// 0x4000): the ONLY reader is client.dll's DVS controller (string xref 0x358e24), which returns it verbatim
		// when dvs_enable is 0 (0x358eb0: cmp [dvs+0x5c],0 / movss xmm0,[viewportscale+0x58]). The caller (0x35baa5)
		// multiplies the main view by that scale exactly like a DVS scale -> an instant, ramp-free render scale.
		cv vps { "mat_viewportscale" };
		cv *all[] = { &dvs, &gmin, &gmax, &smin, &vps };
		bool applied = false;
		int applied_mode = -1, applied_method = -1;
		uint64_t next_try = 0, next_vps_try = 0;

		bool seh_name_ok(const void *var, const char *name)
		{
			__try { const char *n = *reinterpret_cast<const char *const *>(static_cast<const uint8_t *>(var) + 0x18); return n && std::strcmp(n, name) == 0; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		uint8_t *seh_parent(void *var)
		{
			__try { uint8_t *p = *reinterpret_cast<uint8_t **>(static_cast<uint8_t *>(var) + 0x38); return p ? p : static_cast<uint8_t *>(var); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
		}
		bool seh_read(void *var, float *f, int *i)
		{
			__try { uint8_t *p = seh_parent(var); if (!p) return false; *f = *reinterpret_cast<volatile float *>(p + 0x58); *i = *reinterpret_cast<volatile int *>(p + 0x5C); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		bool seh_write(void *var, float f, int i)
		{
			__try { uint8_t *p = seh_parent(var); if (!p) return false; *reinterpret_cast<volatile float *>(p + 0x58) = f; *reinterpret_cast<volatile int *>(p + 0x5C) = i; return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		bool resolve_all()
		{
			if (!tf2nr::cvar::g_icvar) return false;   // set by the menu-convar reader once vstdlib is up
			bool ok = true;
			for (cv *c : all)
			{
				if (c->ptr || c == &vps) continue;   // vps is optional, resolved by resolve_vps()
				void *p = tf2nr::cvar::seh_find_var(tf2nr::cvar::g_icvar, c->name);
				if (p && seh_name_ok(p, c->name))
				{
					c->ptr = p;
					float f; int i;
					if (seh_read(p, &f, &i)) logf("engine convar %s = %g (int %d) at %p", c->name, f, i, p);
				}
				else ok = false;
			}
			return ok;
		}

		void resolve_vps(uint64_t frame)
		{
			if (vps.ptr || !tf2nr::cvar::g_icvar || frame < next_vps_try) return;
			next_vps_try = frame + 120;
			void *p = tf2nr::cvar::seh_find_var(tf2nr::cvar::g_icvar, vps.name);
			if (p && seh_name_ok(p, vps.name))
			{
				vps.ptr = p;
				float f; int i;
				if (seh_read(p, &f, &i)) logf("engine convar %s = %g (int %d) at %p", vps.name, f, i, p);
			}
			else if (frame > 60 * 60)
			{
				static bool logged = false;
				if (!logged) logf("engine convar mat_viewportscale not found - PinMethod 1 falls back to the DVS ramp (PinMethod 0)");
				logged = true;
			}
		}

		void set(cv &c, float v)
		{
			if (!c.ptr) return;
			float f; int i;
			if (!c.have_orig && seh_read(c.ptr, &f, &i)) { c.f0 = f; c.i0 = i; c.have_orig = true; }
			seh_write(c.ptr, v, static_cast<int>(v));
		}
		void restore(cv &c)
		{
			if (c.ptr && c.have_orig) seh_write(c.ptr, c.f0, c.i0);
			c.have_orig = false;
		}

		// once per frame from present
		void update(int mode, uint64_t frame)
		{
			resolve_vps(frame);
			if (frame < next_try) return;
			if (!resolve_all()) { next_try = frame + 120; if (!dvs.ptr || !smin.ptr) return; }
			if (mode > 0)
			{
				const float s = tf2nr::sr_render_scale(static_cast<tf2nr::sr_mode>(mode));   // 1.0 for DLAA
				const int method = (g_cfg.pin_method == 1 && vps.ptr) ? 1 : 0;
				if (method != applied_method && applied)
				{
					// switching pin methods (vps resolved late): put everything back first so no stale pin survives
					for (cv *c : all) restore(*c);
				}
				if (method == 1)
				{
					// dvs_enable 0 -> the controller returns mat_viewportscale verbatim: instant, no ramp, no GPU-time
					// feedback. Re-written every frame, so a cheat-convar revert (FCVAR 0x4000) lasts one frame at most.
					set(dvs, 0.0f);
					set(vps, s);
				}
				else if (mode == 1) set(dvs, 0.0f);   // DLAA: native resolution
				else
				{
					set(dvs, 1.0f); set(gmin, 0.0f); set(gmax, 1.0f);   // the controller always wants less GPU time -> minimum scale
					set(smin, s);
				}
				if (!applied || applied_mode != mode || applied_method != method)
					logf("engine: DLSS SR mode %d -> render scale %.4f via %s (live values)", mode, s,
						method == 1 ? "mat_viewportscale, dvs_enable 0 (instant)" : (mode == 1 ? "dvs_enable 0 (DLAA, native)" : "DVS pin (ramps over seconds)"));
				applied = true; applied_mode = mode; applied_method = method;
			}
			else if (applied)
			{
				for (cv *c : all) restore(*c);
				applied = false; applied_mode = 0; applied_method = -1;
				logf("engine: DLSS SR off -> the player's Adaptive Resolution / mat_viewportscale values restored");
			}
		}
	}

	// ------------------------------------------------------------------ Video-menu NR row on the native route
	// With renodx DX11Source=native, NR runs inside this add-on's DLSS evaluate and the feeder techniques the bridge
	// toggles do not exist. renodx has no API; its NR toggle is F6 (GetAsyncKeyState, no focus check) and it keeps the
	// state in [RenoDX.DLSS5] NeuralUplift. When the menu (tf2nr_dlss5_nr) and NeuralUplift disagree and the game has
	// focus, press F6 once, then check that NeuralUplift followed (3 attempts max).
	namespace nrsync
	{
		int stage = 0;            // 0 idle, 1 key down sent, 2 waiting for renodx to save
		uint64_t at = 0;
		// Edge-triggered: renodx follows the menu row when the row CHANGES (and once after load). An F6 the player
		// presses sticks until the row changes again - never fight a manual toggle.
		int attempts = 0, synced = -1;
		bool f6_was_down = false;
		bool native_route() { return g_native_route; }   // 0.4.1: ini says native AND renodx is loaded (refresh_route)
		int renodx_nr()
		{
			const int live = tf2nr::tuning::live_uplift();   // renodx's live atomic (its ini copy is saved lazily)
			if (live >= 0) return live;
			int v = 1;
			reshade::get_config_value(nullptr, "RenoDX.DLSS5", "NeuralUplift", v);
			return v != 0 ? 1 : 0;
		}
		void key(bool down)
		{
			INPUT in = {}; in.type = INPUT_KEYBOARD; in.ki.wVk = VK_F6; in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
			SendInput(1, &in, sizeof(in));
		}
		void update(uint64_t frame)
		{
			// A physical F6 (renodx's own hotkey) while we are not pressing it = the player's choice: adopt it.
			// renodx saves NeuralUplift lazily, so its config value can lag a manual toggle by seconds.
			const bool f6_down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
			if (f6_down && !f6_was_down && stage == 0 && tf2nr::g_var_nr.ptr)
			{
				if (synced != tf2nr::g_nr_enabled) logf("menu: F6 pressed by the player - menu sync paused until the DLSS 5 row changes");
				synced = tf2nr::g_nr_enabled; attempts = 0;
			}
			f6_was_down = f6_down;
			if ((frame % 15) != 0 && stage == 0) return;
			if (frame < 900) return;   // the convar flips while the profile and launch options load; act once settled
			if (!tf2nr::g_var_nr.ptr || !native_route()) return;
			const int want = tf2nr::g_nr_enabled;
			switch (stage)
			{
			case 0:
				if (want == synced) return;
				if (renodx_nr() == want) { synced = want; attempts = 0; return; }
				if (attempts >= 3)
				{
					logf("menu: renodx NR did not follow F6 after 3 attempts - toggle it with F6 yourself");
					synced = want; attempts = 0; return;
				}
				{
					DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
					if (pid != GetCurrentProcessId()) return;   // never send a key into another program
				}
				key(true); stage = 1; at = frame; ++attempts;
				logf("menu: DLSS 5 NR %s requested on the native route -> F6 to renodx (attempt %d)", want ? "ON" : "OFF", attempts);
				break;
			case 1:
				if (frame - at < 4) return;   // hold long enough for renodx's per-present poll
				key(false); stage = 2; at = frame;
				break;
			case 2:
				if (frame - at < 30) return;
				stage = 0;
				if (renodx_nr() == want) { logf("menu: renodx NR is now %s", want ? "ON" : "OFF"); synced = want; attempts = 0; }
				break;
			}
		}
	}

	// ------------------------------------------------------------------ shader tagging
	constexpr uint32_t TAG_TSAA = 1u, TAG_EPOST = 2u;
	std::unordered_map<uint64_t, uint32_t> g_tags;
	std::unordered_set<uint64_t> g_dumped;

	void on_init_pipeline(device *dev, pipeline_layout, uint32_t count, const pipeline_subobject *subobjects, pipeline p)
	{
		if (subobjects == nullptr || foreign_dev(dev)) return;
		for (uint32_t i = 0; i < count; ++i)
		{
			if (subobjects[i].type != pipeline_subobject_type::pixel_shader) continue;
			const auto *sd = static_cast<const shader_desc *>(subobjects[i].data);
			if (!sd || !sd->code || sd->code_size < 32) continue;
			uint32_t tag = 0;
			if (has_str(sd->code, sd->code_size, "CBufTSAA") && has_str(sd->code, sd->code_size, "VelocityBuffer")) tag |= TAG_TSAA;
			if (has_str(sd->code, sd->code_size, "CBufEnginePost") && has_str(sd->code, sd->code_size, "FBTexture")) tag |= TAG_EPOST;
			if (!tag) continue;
			SR_LOCK();
			g_tags[p.handle] = tag;
			const uint64_t h = fnv1a(sd->code, sd->code_size);
			logf("pipeline %016llx: %s PS %016llx (%zu bytes)", p.handle, (tag & TAG_TSAA) ? "TSAA" : "engine_post", h, sd->code_size);
			if (g_cfg.dump_shaders && (tag & TAG_TSAA) && g_dumped.insert(h).second)
			{
				char name[96]; snprintf(name, sizeof(name), "tf2-dlss-sr-tsaa-%016llx.dxbc", h);
				if (FILE *f = _wfsopen((g_dir / name).c_str(), L"wb", _SH_DENYNO)) { fwrite(sd->code, 1, sd->code_size, f); fclose(f); }
			}
		}
	}
	void on_destroy_pipeline(device *dev, pipeline p) { if (foreign_dev(dev)) return; SR_LOCK_ALWAYS(); g_tags.erase(p.handle); }

	// ------------------------------------------------------------------ tracked state
	struct frame_state
	{
		uint32_t ps_tags = 0;
		resource rt0 = { 0 }, dsv_res = { 0 };
		uint32_t rt_count = 0;
		bool main_pass = false;
		viewport vp = {};                       // last bound viewport 0
		uint32_t render_w = 0, render_h = 0;    // main-pass viewport (= DVS sub-rect)
		uint64_t render_seen_frame = ~0ull;     // frame in which render_w/h were last set (0.1 never reset them)
		resource ps_srv[32] = {};
		resource_view ps_srv_view[32] = {};
		buffer_range ps_cb1 = {}, ps_cb2 = {};   // b1 CBufTSAA, b2 CBufCommonPerCamera (at the TSAA draw)
		uint64_t ps_samp[16] = {};               // PS samplers the GAME bound (ID3D11SamplerState*)
		uint32_t samp_twin_mask = 0;             // slots where our biased twin is bound natively instead
		uint32_t samp_checked_mask = 0;          // slots already handled for the current binding + bias
		int samp_twin_bias_q = 0;                // bias (1/8 steps) of the twins currently bound
		resource overwrite_dst = { 0 };
		uint64_t frames = 0, last_tsaa_frame = 0;
		uint32_t tsaa_draws_this_frame = 0;
		resource epost_fb = { 0 };
	} g_fs;

	// stats per 600-frame window
	struct stats_t
	{
		uint64_t tsaa_frames = 0, tsaa_draws = 0, evals_ok = 0, evals_fail = 0, skipped_nojitter = 0, skipped_deferred = 0, skipped_inputs = 0;
		// 0.2 splits 0.1's "size" skips: settling on a new size, waiting out the re-create cooldown, size unknown /
		// the two size sources disagree, (mode,size) rejected by NGX even at an exact-size feature
		uint64_t skipped_settle = 0, skipped_cooldown = 0, skipped_unknown = 0, skipped_rejected = 0;
		uint64_t size_src_cbuf = 0, size_src_viewport = 0, size_disagree = 0, recreates = 0, reallocs = 0;
		// 0.3: TSAA's own sub-rect (c_fbMaxUVs) vs the dominant main-pass viewport
		uint64_t size_agree = 0, size_tsaa_only = 0, size_tsaa_override = 0, size_tsaa_invalid = 0;
		uint64_t no_tsaa_pinned = 0, fgx_valid_frames = 0, multi_vp_frames = 0, size_legacy_diff = 0;
		float jx_min = 1e9f, jx_max = -1e9f, jy_min = 1e9f, jy_max = -1e9f;
		uint32_t rw_min = 0xFFFFFFFF, rw_max = 0;       // EVALUATED render widths
		uint32_t obs_min = 0xFFFFFFFF, obs_max = 0;     // OBSERVED render widths on every TSAA frame (skipped or not)
		uint64_t samp_twins_bound = 0;
		int last_ngx_result = 0;
	} g_st;
	int g_probe_logs = 0, g_probe_windows = 0;   // 0.3.1: detail dumps = 4 first sightings + 1 per window for 8 windows
	// TSAA-time b2 (0.3.1): c_forceExposure as TSAA reads it, per window
	struct fexp_t { uint64_t frames = 0, forced = 0, invalid = 0; float lo = 1e9f, hi = -1e9f; } g_fexp;

	void on_bind_pipeline(command_list *cmd, pipeline_stage stages, pipeline p)
	{
		if ((stages & pipeline_stage::pixel_shader) == 0 || foreign_cmd(cmd)) return;
		SR_LOCK();
		auto it = g_tags.find(p.handle);
		g_fs.ps_tags = it != g_tags.end() ? it->second : 0;
	}

	void on_bind_viewports(command_list *cmd, uint32_t first, uint32_t count, const viewport *vps)
	{
		if (first != 0 || count == 0 || vps == nullptr || foreign_cmd(cmd)) return;
		SR_LOCK();
		g_fs.vp = vps[0];
		if (g_fs.main_pass && vps[0].max_depth >= 0.999f && vps[0].min_depth <= 0.001f)   // skip the compressed-range viewmodel viewport
		{
			g_fs.render_w = static_cast<uint32_t>(vps[0].width + 0.5f);
			g_fs.render_h = static_cast<uint32_t>(vps[0].height + 0.5f);
			g_fs.render_seen_frame = g_fs.frames;
		}
	}

	void on_push_descriptors(command_list *cmd, shader_stage stages, pipeline_layout, uint32_t, const descriptor_table_update &u)
	{
		if ((stages & shader_stage::pixel) == 0 || u.descriptors == nullptr || foreign_cmd(cmd)) return;
		device *dev = cmd->get_device();
		SR_LOCK();
		if (u.type == descriptor_type::shader_resource_view)
		{
			const auto *views = static_cast<const resource_view *>(u.descriptors);
			for (uint32_t i = 0; i < u.count && u.binding + i < 32; ++i)
			{
				g_fs.ps_srv_view[u.binding + i] = views[i];
				g_fs.ps_srv[u.binding + i] = views[i].handle ? dev->get_resource_from_view(views[i]) : resource { 0 };
			}
		}
		else if (u.type == descriptor_type::constant_buffer)
		{
			const auto *ranges = static_cast<const buffer_range *>(u.descriptors);
			for (uint32_t i = 0; i < u.count; ++i)
			{
				if (u.binding + i == 1) g_fs.ps_cb1 = ranges[i];
				if (u.binding + i == 2) g_fs.ps_cb2 = ranges[i];
			}
		}
		else if (u.type == descriptor_type::sampler)
		{
			// ReShade calls the real PSSetSamplers first, then this event: the game's samplers are bound now, so any
			// twin we had in these slots is gone.
			const auto *samps = static_cast<const sampler *>(u.descriptors);
			for (uint32_t i = 0; i < u.count && u.binding + i < 16; ++i)
			{
				g_fs.ps_samp[u.binding + i] = samps[i].handle;
				g_fs.samp_twin_mask &= ~(1u << (u.binding + i));
				g_fs.samp_checked_mask &= ~(1u << (u.binding + i));
			}
		}
	}

	// ------------------------------------------------------------------ CBufTSAA shadow (only buffers seen as the TSAA b1)
	std::unordered_set<uint64_t> g_tsaa_cbs;
	std::unordered_map<uint64_t, std::pair<void *, uint64_t>> g_mapped;
	struct shadow_t { std::vector<uint8_t> bytes; uint64_t frame = 0; int source = 0; };   // source 1 = Map, 2 = UpdateSubresource
	std::unordered_map<uint64_t, shadow_t> g_cb_shadow;

	void on_map_buffer(device *dev, resource r, uint64_t offset, uint64_t size, map_access access, void **data)
	{
		if (access == map_access::read_only || data == nullptr || *data == nullptr || foreign_dev(dev)) return;
		SR_LOCK();
		if (!g_tsaa_cbs.count(r.handle)) return;
		const auto d = dev->get_resource_desc(r);
		g_mapped[r.handle] = { *data, (size == UINT64_MAX || size == 0) ? d.buffer.size - offset : size };
	}
	void on_unmap_buffer(device *dev, resource r)
	{
		if (foreign_dev(dev)) return;
		SR_LOCK();
		auto it = g_mapped.find(r.handle);
		if (it == g_mapped.end()) return;
		const auto *p = static_cast<const uint8_t *>(it->second.first);
		auto &s = g_cb_shadow[r.handle];
		s.bytes.assign(p, p + it->second.second);   // the app has finished writing (event fires before Unmap)
		s.frame = g_fs.frames; s.source = 1;
		g_mapped.erase(it);
	}
	namespace cam
	{
		extern std::atomic<uint64_t> b2; extern uint64_t size_rejects; void on_update(const void *data);
		extern uint8_t latest[576]; extern bool have_latest; extern uint64_t latest_frame;
	}
	bool on_update_buffer(device *dev, const void *data, resource dst, uint64_t offset, uint64_t size)
	{
		if (!data) return false;
		if (size > 4096)
		{
			// 0.3: TF2 uploads CBufCommonPerCamera (b2) whole with UpdateSubresource(NULL box): ReShade reports size UINT64_MAX.
			// 0.3.1: the handle is compared before any lock (every NULL-box upload in the process lands here), and the copy of
			// 576 bytes from the app's pointer is made only when the destination really is a 576-byte buffer (a D3D11 buffer
			// freed and re-created at the same address with another size must never make us read past the app's data).
			if (offset != 0 || size != UINT64_MAX) return false;
			const uint64_t b2 = cam::b2.load(std::memory_order_relaxed);
			if (b2 == 0 || dst.handle != b2 || foreign_dev(dev)) return false;
			SR_LOCK(false);
			if (dst.handle != cam::b2.load(std::memory_order_relaxed)) return false;
			const auto d = dev->get_resource_desc(dst);
			if (d.type != resource_type::buffer || d.buffer.size != 576) { ++cam::size_rejects; return false; }
			cam::on_update(data);
			return false;
		}
		if (foreign_dev(dev)) return false;
		SR_LOCK(false);
		{
			const uint64_t b2 = cam::b2.load(std::memory_order_relaxed);
			if (b2 != 0 && dst.handle == b2 && offset == 0 && size == 576) cam::on_update(data);
		}
		if (!g_tsaa_cbs.count(dst.handle)) return false;
		auto &s = g_cb_shadow[dst.handle];
		if (s.bytes.size() < offset + size) s.bytes.resize(static_cast<size_t>(offset + size));
		memcpy(s.bytes.data() + offset, data, static_cast<size_t>(size));
		s.frame = g_fs.frames; s.source = 2;
		return false;
	}

	struct tsaa_cb
	{
		float jx = 0, jy = 0, blend = 0; bool valid = false; uint64_t age = ~0ull; int source = 0;
		float fb_max_u = 0, fb_max_v = 0;                  // CBufTSAA c_fbMaxUVs @56: TSAA's own clamp of the scene UV
		bool fbmax_valid = false;                          // = (render sub-rect - 0.5 px) / scene texture size
		// CBufCommonPerCamera (b2) as the TSAA pass sees it: the TSAA shader maps output pixel -> render pixel with
		// c_framebufferViewportScale (@560) and clamps loads to ftoi(c_renderTargetSize (@456) * that scale) - 1.
		float rt_w = 0, rt_h = 0, fbvs_x = 0, fbvs_y = 0; bool cam_valid = false; uint64_t cam_age = ~0ull;
		float force_exposure = 0;   // CBufCommonPerCamera c_forceExposure @524: TSAA uses it instead of t30 when > 0
	};
	tsaa_cb read_cbuf_tsaa()
	{
		tsaa_cb out;
		auto it = g_cb_shadow.find(g_fs.ps_cb1.buffer.handle);
		const uint64_t off = g_fs.ps_cb1.offset;
		if (it != g_cb_shadow.end() && it->second.bytes.size() >= off + 12)
		{
			memcpy(&out.jx, it->second.bytes.data() + off + 0, 4);      // c_jitterX @0
			memcpy(&out.jy, it->second.bytes.data() + off + 4, 4);      // c_jitterY @4
			memcpy(&out.blend, it->second.bytes.data() + off + 8, 4);   // c_curFrameBlendAmount @8
			if (it->second.bytes.size() >= off + 64)
			{
				memcpy(&out.fb_max_u, it->second.bytes.data() + off + 56, 4);
				memcpy(&out.fb_max_v, it->second.bytes.data() + off + 60, 4);
			}
			out.age = g_fs.frames - it->second.frame;
			out.source = it->second.source;
			out.valid = out.age <= 1 && std::isfinite(out.jx) && std::isfinite(out.jy) && std::fabs(out.jx) < 0.05f && std::fabs(out.jy) < 0.05f;
			out.fbmax_valid = out.valid && it->second.bytes.size() >= off + 64 && std::isfinite(out.fb_max_u) && std::isfinite(out.fb_max_v) &&
			                  out.fb_max_u > 0.001f && out.fb_max_v > 0.001f && out.fb_max_u <= 1.0001f && out.fb_max_v <= 1.0001f;
		}
		// b2 as the TSAA pass sees it. TF2 uploads CBufCommonPerCamera whole with a NULL-box UpdateSubresource once per view,
		// so the latest upload before this draw IS the buffer TSAA reads (0.3.1; the Map/partial-update shadow below never
		// sees b2, which left c_forceExposure unread in 0.3.0).
		const uint64_t b2 = cam::b2.load(std::memory_order_relaxed);
		auto ic = g_cb_shadow.find(g_fs.ps_cb2.buffer.handle);
		const uint64_t o2 = g_fs.ps_cb2.offset;
		const bool from_upload = g_fs.ps_cb2.buffer.handle != 0 && g_fs.ps_cb2.buffer.handle == b2 && o2 == 0 && cam::have_latest;
		if (from_upload || (g_fs.ps_cb2.buffer.handle && ic != g_cb_shadow.end() && ic->second.bytes.size() >= o2 + 568))
		{
			const uint8_t *b = from_upload ? cam::latest : ic->second.bytes.data() + o2;
			memcpy(&out.rt_w, b + 456, 4); memcpy(&out.rt_h, b + 460, 4);
			memcpy(&out.fbvs_x, b + 560, 4); memcpy(&out.fbvs_y, b + 564, 4);
			memcpy(&out.force_exposure, b + 524, 4);
			if (!std::isfinite(out.force_exposure) || out.force_exposure > 1.0e6f) out.force_exposure = 0;
			const uint64_t at = from_upload ? cam::latest_frame : ic->second.frame;
			out.cam_age = g_fs.frames >= at ? g_fs.frames - at : ~0ull;
			if (out.cam_age > 1) out.force_exposure = 0;   // a stale buffer never forces DLSS's exposure
			out.cam_valid = out.cam_age <= 1 && std::isfinite(out.rt_w) && std::isfinite(out.rt_h) && std::isfinite(out.fbvs_x) && std::isfinite(out.fbvs_y) &&
			                out.rt_w >= 16.0f && out.rt_h >= 16.0f && out.rt_w <= 16384.0f && out.rt_h <= 16384.0f &&
			                out.fbvs_x > 0.01f && out.fbvs_y > 0.01f && out.fbvs_x <= 1.0001f && out.fbvs_y <= 1.0001f;
		}
		return out;
	}

	// ------------------------------------------------------------------ render size
	// Main-pass viewport histogram of THIS frame: every draw of the main scene pass (RGBA16 + RG16 MRT) with a full depth
	// range viewport (the viewmodel's compressed-range viewport is skipped) at the origin counts for its viewport size.
	struct vp_hist_t { uint32_t w[4] = {}, h[4] = {}, draws[4] = {}; uint32_t n = 0, other = 0; uint64_t frame = ~0ull; } g_vph;
	void note_main_draw()
	{
		const viewport &v = g_fs.vp;
		if (!(v.max_depth >= 0.999f && v.min_depth <= 0.001f)) return;
		if (!(v.width >= 1.0f && v.height >= 1.0f && v.width <= 16384.0f && v.height <= 16384.0f)) return;
		if (g_vph.frame != g_fs.frames) { g_vph.n = 0; g_vph.other = 0; g_vph.frame = g_fs.frames; }
		if (std::fabs(v.x) > 0.5f || std::fabs(v.y) > 0.5f) { ++g_vph.other; return; }
		const uint32_t w = static_cast<uint32_t>(v.width + 0.5f), h = static_cast<uint32_t>(v.height + 0.5f);
		for (uint32_t i = 0; i < g_vph.n; ++i)
			if (g_vph.w[i] == w && g_vph.h[i] == h) { ++g_vph.draws[i]; return; }
		if (g_vph.n < 4) { g_vph.w[g_vph.n] = w; g_vph.h[g_vph.n] = h; g_vph.draws[g_vph.n] = 1; ++g_vph.n; }
		else ++g_vph.other;
	}
	bool main_draws_this_frame() { return g_vph.frame == g_fs.frames && g_vph.n > 0; }
	bool dominant_vp(uint32_t &w, uint32_t &h)
	{
		if (!main_draws_this_frame()) return false;
		uint32_t best = 0;
		for (uint32_t i = 1; i < g_vph.n; ++i) if (g_vph.draws[i] > g_vph.draws[best]) best = i;
		w = g_vph.w[best]; h = g_vph.h[best];
		return w > 0 && h > 0;
	}
	void vph_str(char *buf, size_t n)
	{
		buf[0] = '\0';
		if (!main_draws_this_frame()) { snprintf(buf, n, "none"); return; }
		size_t used = 0;
		for (uint32_t i = 0; i < g_vph.n && used + 1 < n; ++i)
		{
			const int k = snprintf(buf + used, n - used, "%s%ux%u (%u draws)", i ? ", " : "", g_vph.w[i], g_vph.h[i], g_vph.draws[i]);
			if (k < 0 || static_cast<size_t>(k) >= n - used) return;   // truncated: buffer is full and terminated
			used += static_cast<size_t>(k);
		}
		if (g_vph.other && used + 1 < n) snprintf(buf + used, n - used, ", other %u", g_vph.other);
	}

	// TSAA's own render sub-rect: the TSAA shader clamps its scene UV to CBufTSAA c_fbMaxUVs = (sub-rect - 0.5 px) / scene
	// texture size (1113.5/1920 at Balanced) and maps output pixels into that sub-rect with c_framebufferViewportScale.
	// This is exactly the image the game itself upscales, so it is what DLSS must get.
	bool tsaa_size(const tsaa_cb &tcb, uint32_t sw, uint32_t sh, uint32_t &w, uint32_t &h)
	{
		if (!tcb.fbmax_valid || sw == 0 || sh == 0) return false;
		const float fw = std::floor(tcb.fb_max_u * static_cast<float>(sw) + 1.0f);   // (w - 0.5) + 0.5, rounded
		const float fh = std::floor(tcb.fb_max_v * static_cast<float>(sh) + 1.0f);
		if (!(fw >= 1.0f && fh >= 1.0f)) return false;
		w = fw > static_cast<float>(sw) ? sw : static_cast<uint32_t>(fw);
		h = fh > static_cast<float>(sh) ? sh : static_cast<uint32_t>(fh);
		return true;
	}

	// The render size DLSS gets for this frame (0.3). 0.2 took the LAST main-pass viewport and meant to cross-check it with
	// the TSAA pass's b2 constants, but TF2 never updates CBufCommonPerCamera through Map/UpdateSubresource, so the check
	// never ran; during the death cam a full-size viewport is bound in the main pass while the scene still sits in the DVS
	// sub-rect, and DLSS then got the whole 1920x1200 RT (scene in the top-left corner, stale content around it).
	// SizeSource 0 (auto): TSAA's c_fbMaxUVs sub-rect; when the dominant main-pass viewport agrees (+-2 px) its exact integer
	// size is used, when it disagrees TSAA's wins (logged). No valid CBufTSAA -> the dominant viewport. 1 viewport only,
	// 2 b2 CPU shadow only, 3 TSAA only. `from_tsaa` = TSAA's size won a disagreement.
	bool resolve_render_size(const tsaa_cb &tcb, uint32_t sw, uint32_t sh, uint32_t ow, uint32_t oh, uint32_t &rw, uint32_t &rh, bool &from_tsaa)
	{
		from_tsaa = false;
		uint32_t vw = 0, vh = 0, tw = 0, th = 0;
		const bool vp_ok = dominant_vp(vw, vh);
		const bool t_ok = tsaa_size(tcb, sw, sh, tw, th);
		if (!t_ok) ++g_st.size_tsaa_invalid;
		switch (g_cfg.size_source)
		{
		case 1:
			if (!vp_ok) return false;
			rw = vw; rh = vh; ++g_st.size_src_viewport;
			break;
		case 2:
			if (!tcb.cam_valid) return false;
			rw = static_cast<uint32_t>(std::floor(tcb.rt_w * tcb.fbvs_x + 0.5f));
			rh = static_cast<uint32_t>(std::floor(tcb.rt_h * tcb.fbvs_y + 0.5f));
			++g_st.size_src_cbuf;
			break;
		case 3:
			if (!t_ok) return false;
			rw = tw; rh = th; ++g_st.size_tsaa_only;
			break;
		default:
			if (t_ok)
			{
				const int dw = static_cast<int>(vw) - static_cast<int>(tw), dh = static_cast<int>(vh) - static_cast<int>(th);
				if (vp_ok && dw >= -2 && dw <= 2 && dh >= -2 && dh <= 2) { rw = vw; rh = vh; ++g_st.size_agree; }
				else
				{
					rw = tw; rh = th;
					if (!vp_ok) ++g_st.size_tsaa_only;
					else
					{
						from_tsaa = true;
						static uint64_t s_logged = 0, s_last_frame = 0;
						++g_st.size_tsaa_override;
						if (s_logged < 20 || g_fs.frames >= s_last_frame + 600)
						{
							char vb[192]; vph_str(vb, sizeof(vb));
							logf("render size: frame %llu TSAA c_fbMaxUVs %.6f,%.6f x %ux%u -> sub-rect %ux%u, but main-pass viewports this frame: %s -> using TSAA's %ux%u",
								g_fs.frames, tcb.fb_max_u, tcb.fb_max_v, sw, sh, tw, th, vb, tw, th);
							++s_logged; s_last_frame = g_fs.frames;
						}
					}
				}
			}
			else if (vp_ok) { rw = vw; rh = vh; ++g_st.size_src_viewport; }
			else return false;
			break;
		}
		if (rw == 0 || rh == 0) return false;
		rw = rw > ow ? ow : rw; rh = rh > oh ? oh : rh;
		rw = rw > sw ? sw : rw; rh = rh > sh ? sh : rh;   // prep_cs reads [0, rw) x [0, rh) of the scene texture
		return true;
	}

	// ------------------------------------------------------------------ P0 probe log
	const char *fmt_name(format f)
	{
		switch (f)
		{
		case format::r16g16b16a16_typeless: return "RGBA16_TYPELESS";
		case format::r16g16b16a16_float: return "RGBA16F";
		case format::r16g16_typeless: return "RG16_TYPELESS";
		case format::r16g16_float: return "RG16F";
		case format::r24_g8_typeless: return "R24G8_TYPELESS";
		case format::r24_unorm_x8_uint: return "R24_UNORM_X8";
		case format::r32_float: return "R32F";
		case format::r32_typeless: return "R32_TYPELESS";
		case format::r16_typeless: return "R16_TYPELESS";
		case format::r16_float: return "R16F";
		case format::r8g8b8a8_typeless: return "RGBA8_TYPELESS";
		case format::r8g8b8a8_unorm: return "RGBA8";
		case format::r8g8b8a8_unorm_srgb: return "RGBA8_SRGB";
		case format::r11g11b10_float: return "R11G11B10F";
		case format::r8_typeless: return "R8_TYPELESS";
		case format::r8_unorm: return "R8";
		default: return nullptr;
		}
	}

	void describe(device *dev, resource r, char *buf, size_t n)
	{
		if (!r.handle) { snprintf(buf, n, "null"); return; }
		const auto d = dev->get_resource_desc(r);
		const char *fn = fmt_name(d.texture.format);
		if (d.type == resource_type::buffer) snprintf(buf, n, "buffer %llu B", d.buffer.size);
		else if (fn) snprintf(buf, n, "%ux%u %s", d.texture.width, d.texture.height, fn);
		else snprintf(buf, n, "%ux%u fmt %u", d.texture.width, d.texture.height, static_cast<uint32_t>(d.texture.format));
	}

	void probe_log(command_list *cmd, const tsaa_cb &tcb, const char *why)
	{
		device *dev = cmd->get_device();
		auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native());
		char b[128];
		describe(dev, g_fs.rt0, b, sizeof(b));
		logf("TSAA %s: frame %llu draw %u ctx %s | rt0 %016llx %s (%u RTVs) | viewport %.0fx%.0f @%.0f,%.0f | main-pass viewport %ux%u | engine_post FB %s",
			why, g_fs.frames, g_fs.tsaa_draws_this_frame, ctx && ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE ? "immediate" : "DEFERRED",
			g_fs.rt0.handle, b, g_fs.rt_count, g_fs.vp.width, g_fs.vp.height, g_fs.vp.x, g_fs.vp.y, g_fs.render_w, g_fs.render_h,
			g_fs.epost_fb.handle == 0 ? "not seen yet" : (g_fs.epost_fb.handle == g_fs.rt0.handle ? "== TSAA rt0" : "!= TSAA rt0"));
		// 0.3.1: only the TSAA pass's own inputs (scene t0, history t1, velocity t4, depth t5, exposure t30): the game binds
		// them for this draw, so their handles are live. Other slots may hold handles we never saw unbound.
		static const uint32_t kSlots[] = { 0, 1, 4, 5, 30 };
		for (const uint32_t s : kSlots)
		{
			if (!g_fs.ps_srv[s].handle) continue;
			describe(dev, g_fs.ps_srv[s], b, sizeof(b));
			const auto vd = dev->get_resource_view_desc(g_fs.ps_srv_view[s]);
			const char *vf = fmt_name(vd.format);
			logf("   t%-2u %016llx %s view %s", s, g_fs.ps_srv[s].handle, b, vf ? vf : "?");
		}
		logf("   b1 buffer %016llx offset %llu size %llu | c_jitter %.7f %.7f (x render %u: %.3f px, x full %.0f: %.3f px) blend %.4f | shadow age %llu source %s",
			g_fs.ps_cb1.buffer.handle, g_fs.ps_cb1.offset, g_fs.ps_cb1.size, tcb.jx, tcb.jy, g_fs.render_w, tcb.jx * g_fs.render_w,
			g_fs.vp.width, tcb.jx * g_fs.vp.width, tcb.blend, tcb.age, tcb.source == 1 ? "Map" : (tcb.source == 2 ? "UpdateSubresource" : "none"));
		logf("   b1 c_fbMaxUVs %.6f %.6f (x rt0: %.2f x %.2f) | b2 buffer %016llx offset %llu: c_renderTargetSize %.1f x %.1f c_framebufferViewportScale %.6f %.6f -> %.2f x %.2f, age %llu %s | viewport seen this frame: %s",
			tcb.fb_max_u, tcb.fb_max_v, tcb.fb_max_u * g_fs.vp.width, tcb.fb_max_v * g_fs.vp.height, g_fs.ps_cb2.buffer.handle, g_fs.ps_cb2.offset,
			tcb.rt_w, tcb.rt_h, tcb.fbvs_x, tcb.fbvs_y, tcb.rt_w * tcb.fbvs_x, tcb.rt_h * tcb.fbvs_y, tcb.cam_age, tcb.cam_valid ? "valid" : "INVALID",
			g_fs.render_seen_frame == g_fs.frames ? "yes" : "NO (stale)");
	}

	// ------------------------------------------------------------------ our D3D11 objects
	template <class T> void safe_release(T *&p) { if (p) { p->Release(); p = nullptr; } }

	struct gpu_t
	{
		ID3D11Device *dev = nullptr;
		ID3D11ComputeShader *cs = nullptr;
		ID3D11Buffer *params = nullptr;
		ID3D11Texture2D *color = nullptr, *mv = nullptr, *depth = nullptr, *out = nullptr, *exposure = nullptr;
		ID3D11UnorderedAccessView *color_uav = nullptr, *mv_uav = nullptr, *depth_uav = nullptr;
		uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
	} g_gpu;

	// ------------------------------------------------------------------ FG export snapshot (tf2sr_get_fg_inputs)
	// Written on the render thread right after a successful DLSS evaluate (under g_mx), read by the export from any thread.
	// fgx::mx is the only lock the export takes (never g_mx), and it is always taken AFTER g_mx on our side.
	namespace fgx
	{
		std::mutex mx;
		struct snap_t
		{
			bool valid = false;
			uint64_t frame = 0, serial = 0, generation = 0, camera_frame = 0;
			ID3D11Texture2D *depth = nullptr, *mv = nullptr, *expo = nullptr;   // our own references
			uint32_t rw = 0, rh = 0, ow = 0, oh = 0, trw = 0, trh = 0, flags = 0;
			int mode = 0;
			float jx_px = 0, jy_px = 0, jx_uv = 0, jy_uv = 0, expo_forced = 0;
			bool cam_valid = false, cam_far_inf = false;
			float cam_near = 0, cam_far = 0, cam_fovy = 0, cam_aspect = 0;
		} s;
		std::atomic<uint64_t> sr_frame { 0 };              // mirror of g_fs.frames for the export
		std::atomic<uint64_t> calls { 0 }, calls_valid { 0 }, calls_bad { 0 };
		uint64_t generation = 1;                            // bumped on every (re)allocation / release of the textures
		uint64_t serial = 0;

		void hold(ID3D11Texture2D *&dst, ID3D11Texture2D *src)
		{
			if (dst == src) return;
			if (src) src->AddRef();
			if (dst) dst->Release();
			dst = src;
		}
		void drop()   // textures released / reallocated: forget our references
		{
			std::lock_guard lk(mx);
			hold(s.depth, nullptr); hold(s.mv, nullptr); hold(s.expo, nullptr);
			s.valid = false;
			++generation;
		}
		void invalidate() { std::lock_guard lk(mx); s.valid = false; }
	}

	// ------------------------------------------------------------------ camera (CBufCommonPerCamera, b2) for the FG export
	// TF2 uploads the whole 576-byte b2 buffer with UpdateSubresource(NULL box) once per view (shadow views, main view,
	// viewmodel, 2D/post); ReShade 6.8 reports those updates with size UINT64_MAX (0.2's shadow dropped them as too big).
	// The latest upload is kept (it IS what the GPU reads at the next draw), and the one current at the FIRST draw of the main
	// scene pass (the main view) is decoded with research\fg_hdr\camera_params.md section 4:
	//   C = c_cameraRelativeToClip (row_major, row i at 16 + 16 i; clip_i = dot(C[i].xyz, p) + C[i][3]), r_i = C[i].xyz
	//   L3 = |r3|^2 (1 for a perspective view), k = dot(r2, r3) / L3 (= f / (f - n); exactly 1 for TF2's infinite projection)
	//   near = -C[2][3] / k, tanHalfY = sqrt(L3 / (|r1|^2 - dot(r1, r3)^2 / L3)) (jitter term removed), same for X with r0
	// Gates: perspective (0.99 < L3 < 1.01, |C33| < 1e-4), 0.5 < near < 64 in, c_viewportMaxZ (@540) >= 0.999 (not the
	// viewmodel range), c_zNear (@0) / c_viewportMaxZ == near within 2 %, and at publish time c_renderTargetSize (@456) *
	// c_viewportScale (@544) == this frame's DLSS render size +-2 px. The 0.3.0 GPU readback at the TSAA draw read the
	// 2D/post state (orthographic, C33 = 1), not the main view.
	namespace cam
	{
		std::atomic<uint64_t> b2 { 0 };           // CBufCommonPerCamera (learned at the TSAA draw; must be a 576-byte buffer)
		uint8_t latest[576] = {};
		bool have_latest = false, snapped = false;
		uint64_t latest_frame = 0;                 // g_fs.frames of the latest upload
		uint64_t updates = 0, snaps = 0, ok_count = 0, bad_count = 0, no_update = 0, size_mismatch = 0, published = 0, size_rejects = 0;
		const char *last_reason = "none";
		struct result_t
		{
			bool valid = false, far_inf = false;
			uint64_t frame = ~0ull;
			float znear_cb = 0, vp_maxz = 0, znear = 0, zfar = 0, fovy = 0, aspect = 0, k = 0, l3 = 0, c33 = 0;
			float rt_w = 0, rt_h = 0, vps_x = 0, vps_y = 0, fbvs_x = 0, fbvs_y = 0, force_exposure = 0;
			// 0.3.1 camera-cut hint: world position (c_cameraOrigin @4), view direction (C row 3 / |r3|), engine's previous-frame
			// origin (c_cameraOriginPrevFrame @84); deltas against the previous frame's main view
			float org[3] = {}, fwd[3] = {}, eng_prev_org[3] = {};
			bool pose_ok = false, have_delta = false, cut = false;
			float dpos = 0, dang = 0, dfov = 0, eng_prev_d = 0;   // units, degrees, degrees; |engine prev origin - our prev origin|
		} last, frame_res, prev;
		// per window: cut hints and the largest deltas of frames WITHOUT a cut (the margin to the thresholds)
		uint64_t cuts = 0, deltas = 0, cuts_logged = 0;
		float max_dpos = 0, max_dang = 0, max_dfov = 0;

		void release()
		{
			b2.store(0); have_latest = false; snapped = false; last = {}; frame_res = {}; prev = {};
		}

		// update_buffer_region on b2 (under g_mx, size checked by the caller): materialsystem's 576-byte shadow, the same bytes
		// D3D reads at the next draw.
		void on_update(const void *data)
		{
			memcpy(latest, data, sizeof(latest));
			have_latest = true;
			latest_frame = g_fs.frames;
			++updates;
		}

		bool decode(const uint8_t *p, result_t &r, const char *&why)
		{
			float c[4][4];
			memcpy(c, p + 16, 64);
			memcpy(&r.znear_cb, p + 0, 4);
			memcpy(&r.rt_w, p + 456, 4); memcpy(&r.rt_h, p + 460, 4);
			memcpy(&r.force_exposure, p + 524, 4);
			memcpy(&r.vp_maxz, p + 540, 4);
			memcpy(&r.vps_x, p + 544, 4); memcpy(&r.vps_y, p + 548, 4);
			memcpy(&r.fbvs_x, p + 560, 4); memcpy(&r.fbvs_y, p + 564, 4);
			memcpy(r.org, p + 4, 12);
			memcpy(r.eng_prev_org, p + 84, 12);
			for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) if (!std::isfinite(c[i][j])) { why = "matrix not finite"; return false; }
			auto dot3 = [&](int a, int b) { return c[a][0] * c[b][0] + c[a][1] * c[b][1] + c[a][2] * c[b][2]; };
			r.l3 = dot3(3, 3);
			r.c33 = c[3][3];
			if (!(r.l3 > 0.99f && r.l3 < 1.01f) || !(std::fabs(r.c33) < 1e-4f)) { why = "not a perspective view (L3 / C33)"; return false; }
			r.k = dot3(2, 3) / r.l3;
			if (!(r.k > 0.5f && r.k < 1.5f)) { why = "depth scale k out of range"; return false; }
			r.znear = -c[2][3] / r.k;
			if (!(r.znear > 0.5f && r.znear < 64.0f)) { why = "near out of 0.5..64"; return false; }
			const float d13 = dot3(1, 3), d03 = dot3(0, 3);
			const float den_y = dot3(1, 1) - d13 * d13 / r.l3, den_x = dot3(0, 0) - d03 * d03 / r.l3;
			if (!(den_y > 1e-8f) || !(den_x > 1e-8f)) { why = "degenerate rows"; return false; }
			const float tan_y = std::sqrt(r.l3 / den_y), tan_x = std::sqrt(r.l3 / den_x);
			if (!std::isfinite(tan_y) || !std::isfinite(tan_x) || !(tan_y > 0.01f) || !(tan_x > 0.01f)) { why = "fov out of range"; return false; }
			r.fovy = 2.0f * std::atan(tan_y);
			r.aspect = tan_x / tan_y;
			if (r.k >= 0.99999f) { r.far_inf = true; r.zfar = 1.0e7f; }
			else
			{
				r.zfar = r.znear * r.k / (r.k - 1.0f);   // k < 0.99999 here: no division by ~0
				if (!std::isfinite(r.zfar) || !(r.zfar > r.znear * 1.001f) || r.zfar > 1.0e7f) { r.far_inf = true; r.zfar = 1.0e7f; }
			}
			if (!(r.vp_maxz >= 0.999f)) { why = "viewport MaxZ < 1 (viewmodel range)"; return false; }
			if (!(std::fabs(r.znear_cb / r.vp_maxz - r.znear) <= 0.02f * r.znear + 1e-3f)) { why = "c_zNear disagrees with the matrix"; return false; }
			if (!(r.fovy > 0.05f && r.fovy < 3.1f) || !std::isfinite(r.aspect)) { why = "fov out of range"; return false; }
			// pose for the cut hint: C row 3 = the view's forward axis (w = view z; |r3| = L3 ~ 1), origin in world units
			const float inv = 1.0f / std::sqrt(r.l3);   // l3 in (0.99, 1.01) here
			for (int i = 0; i < 3; ++i) r.fwd[i] = c[3][i] * inv;
			r.pose_ok = std::isfinite(r.org[0]) && std::isfinite(r.org[1]) && std::isfinite(r.org[2]) &&
			            std::fabs(r.org[0]) < 1.0e7f && std::fabs(r.org[1]) < 1.0e7f && std::fabs(r.org[2]) < 1.0e7f;
			return true;
		}

		// Camera-cut hint (0.3.1): this frame's main view against the previous frame's (consecutive frames only).
		void cut_hint(result_t &r)
		{
			if (!r.pose_ok || !prev.valid || !prev.pose_ok || prev.frame == ~0ull || prev.frame + 1 != r.frame) return;
			const float dx = r.org[0] - prev.org[0], dy = r.org[1] - prev.org[1], dz = r.org[2] - prev.org[2];
			r.dpos = std::sqrt(dx * dx + dy * dy + dz * dz);
			float d = r.fwd[0] * prev.fwd[0] + r.fwd[1] * prev.fwd[1] + r.fwd[2] * prev.fwd[2];
			d = d > 1.0f ? 1.0f : (d < -1.0f ? -1.0f : d);
			r.dang = std::acos(d) * 57.29578f;
			r.dfov = std::fabs(r.fovy - prev.fovy) * 57.29578f;
			const float ex = r.eng_prev_org[0] - prev.org[0], ey = r.eng_prev_org[1] - prev.org[1], ez = r.eng_prev_org[2] - prev.org[2];
			r.eng_prev_d = std::sqrt(ex * ex + ey * ey + ez * ez);
			if (!std::isfinite(r.dpos) || !std::isfinite(r.dang) || !std::isfinite(r.dfov)) return;
			r.have_delta = true;
			++deltas;
			r.cut = r.dpos > g_cfg.cam_cut_dist || r.dang > g_cfg.cam_cut_deg || r.dfov > g_cfg.cam_cut_fov_deg;
			if (r.cut)
			{
				++cuts;
				if (cuts_logged++ < 60)
					logf("camera cut hint: frame %llu moved %.1f units, turned %.1f deg, fovY %+.2f deg (%.2f -> %.2f) | engine's previous-frame origin is %.1f units from our previous main view",
						r.frame, r.dpos, r.dang, (r.fovy - prev.fovy) * 57.29578f, prev.fovy * 57.29578f, r.fovy * 57.29578f, r.eng_prev_d);
			}
			else
			{
				if (r.dpos > max_dpos) max_dpos = r.dpos;
				if (r.dang > max_dang) max_dang = r.dang;
				if (r.dfov > max_dfov) max_dfov = r.dfov;
			}
		}

		// First draw of the main scene pass (under g_mx): the latest b2 upload is the main view's.
		void snap_main()
		{
			if (snapped) return;
			snapped = true;
			++snaps;
			result_t r;
			r.frame = g_fs.frames;
			if (!have_latest) { ++no_update; last_reason = "no b2 upload seen yet"; frame_res = r; return; }
			const char *why = "";
			r.valid = decode(latest, r, why);
			if (r.valid) { ++ok_count; cut_hint(r); prev = r; } else { ++bad_count; last_reason = why; }
			frame_res = r;
			last = r;
		}

		void end_frame() { snapped = false; }
	}

	bool make_tex(uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind, ID3D11Texture2D **tex, ID3D11UnorderedAccessView **uav)
	{
		D3D11_TEXTURE2D_DESC d = {};
		d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
		if (FAILED(g_gpu.dev->CreateTexture2D(&d, nullptr, tex))) return false;
		return uav == nullptr || SUCCEEDED(g_gpu.dev->CreateUnorderedAccessView(*tex, nullptr, uav));
	}

	void release_targets()
	{
		fgx::drop();   // the export's own references go first (+ generation bump); a holder (tf2-fg) keeps its AddRef'd copies alive
		safe_release(g_gpu.color_uav); safe_release(g_gpu.mv_uav); safe_release(g_gpu.depth_uav);
		safe_release(g_gpu.color); safe_release(g_gpu.mv); safe_release(g_gpu.depth); safe_release(g_gpu.out);
		g_gpu.rw = g_gpu.rh = g_gpu.ow = g_gpu.oh = 0;
	}

	// Inputs are allocated at exactly the render size, so colour, depth and motion agree for renodx's pre-SR NR
	// (with display-size guides it reported "guides 1920x1200" against a 1280x800 input; renodx rebuilds its NR
	// working set whenever the input size changes, which is why run_dlss settles + rate-limits size changes).
	bool targets_match(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh)
	{
		return g_gpu.out && g_gpu.color && g_gpu.rw == rw && g_gpu.rh == rh && g_gpu.ow == ow && g_gpu.oh == oh;
	}
	bool ensure_targets(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh)
	{
		if (targets_match(rw, rh, ow, oh)) return true;
		release_targets();
		const UINT srv_uav = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		bool ok = make_tex(rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT, srv_uav, &g_gpu.color, &g_gpu.color_uav) &&
		          make_tex(rw, rh, DXGI_FORMAT_R16G16_FLOAT, srv_uav, &g_gpu.mv, &g_gpu.mv_uav) &&
		          make_tex(rw, rh, DXGI_FORMAT_R32_FLOAT, srv_uav, &g_gpu.depth, &g_gpu.depth_uav) &&
		          make_tex(ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT, srv_uav, &g_gpu.out, nullptr);
		if (ok && !g_gpu.exposure)
			ok = make_tex(1, 1, DXGI_FORMAT_R16_FLOAT, D3D11_BIND_SHADER_RESOURCE, &g_gpu.exposure, nullptr);
		if (ok) { g_gpu.rw = rw; g_gpu.rh = rh; g_gpu.ow = ow; g_gpu.oh = oh; }
		else release_targets();
		++g_st.reallocs;
		logf("targets: inputs %ux%u, output %ux%u: %s", rw, rh, ow, oh, ok ? "ok" : "FAILED");
		return ok;
	}

	std::unordered_map<uint64_t, ID3D11ShaderResourceView *> g_srv_cache;
	ID3D11ShaderResourceView *typed_srv(resource r, DXGI_FORMAT fmt)
	{
		auto it = g_srv_cache.find(r.handle);
		if (it != g_srv_cache.end()) return it->second;
		D3D11_SHADER_RESOURCE_VIEW_DESC d = {};
		d.Format = fmt; d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; d.Texture2D.MipLevels = 1;
		ID3D11ShaderResourceView *srv = nullptr;
		g_gpu.dev->CreateShaderResourceView(reinterpret_cast<ID3D11Resource *>(r.handle), &d, &srv);
		g_srv_cache[r.handle] = srv;
		return srv;
	}
	void on_destroy_resource(device *dev, resource r)
	{
		if (r.handle == 0 || foreign_dev(dev)) return;
		SR_LOCK_ALWAYS();
		auto it = g_srv_cache.find(r.handle);
		if (it != g_srv_cache.end()) { safe_release(it->second); g_srv_cache.erase(it); }
		g_cb_shadow.erase(r.handle);
		g_tsaa_cbs.erase(r.handle);
		if (g_fs.overwrite_dst.handle == r.handle) g_fs.overwrite_dst = { 0 };
		// 0.3.1: never keep a destroyed buffer/texture as b2, a bound CB or an SRV slot (a new object may reuse the address)
		if (r.handle == cam::b2.load(std::memory_order_relaxed))
		{
			cam::b2.store(0); cam::have_latest = false;
			logf("camera: CBufCommonPerCamera %016llx destroyed by the game - forgotten until the TSAA draw learns b2 again", r.handle);
		}
		if (g_fs.ps_cb1.buffer.handle == r.handle) g_fs.ps_cb1 = {};
		if (g_fs.ps_cb2.buffer.handle == r.handle) g_fs.ps_cb2 = {};
		for (uint32_t s = 0; s < 32; ++s)
			if (g_fs.ps_srv[s].handle == r.handle) { g_fs.ps_srv[s] = { 0 }; g_fs.ps_srv_view[s] = { 0 }; }
		if (g_fs.rt0.handle == r.handle) g_fs.rt0 = { 0 };
		if (g_fs.epost_fb.handle == r.handle) g_fs.epost_fb = { 0 };
	}

	// ------------------------------------------------------------------ NGX
	struct ngx_t
	{
		bool inited = false, failed = false;
		NVSDK_NGX_Parameter *params = nullptr;
		NVSDK_NGX_Handle *feature = nullptr;
		uint32_t ow = 0, oh = 0; int quality = -1, flags = -1, preset = -1, mode = -1;
		uint32_t cw = 0, ch = 0;           // creation InWidth/InHeight = the largest render sub-rect this feature takes
		uint32_t lo_w = 0, lo_h = 0;       // smallest render sub-rect it takes (see ngx_range)
		bool need_reset = true;
		bool force_exact = false;          // last evaluate was rejected at a sub-rect != creation size: re-create at it
	} g_ngx;

	uint64_t g_last_eval_frame = 0;        // last frame DLSS produced the image (mip bias only while it does)
	uint32_t g_obs_rw = 0, g_obs_ow = 0;   // last resolved render width / display width

	// Render sizes NGX rejected with 0xbad00005 even with a feature created at exactly that size (mode, w, h).
	std::unordered_set<uint64_t> g_rejected;
	uint64_t rej_key(int mode, uint32_t w, uint32_t h) { return (static_cast<uint64_t>(mode) << 40) | (static_cast<uint64_t>(w) << 20) | h; }

	// NGX's own view of a mode (NGX_DLSS_GET_OPTIMAL_SETTINGS): optimal + dynamic min/max render size. Logged once per
	// (output, mode); only the dynamic MIN is used to narrow the accepted range (see ngx_range for why not the max).
	struct optimal_t { bool ok = false; uint32_t opt_w = 0, opt_h = 0, max_w = 0, max_h = 0, min_w = 0, min_h = 0; float sharp = 0; };
	std::unordered_map<uint64_t, optimal_t> g_optimal;
	optimal_t query_optimal(uint32_t ow, uint32_t oh, int mode)
	{
		const uint64_t key = rej_key(mode, ow, oh);
		auto it = g_optimal.find(key);
		if (it != g_optimal.end()) return it->second;
		optimal_t o;
		const int q = tf2nr::sr_ngx_perf_quality(static_cast<tf2nr::sr_mode>(mode));
		const NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(g_ngx.params, ow, oh, static_cast<NVSDK_NGX_PerfQuality_Value>(q),
			&o.opt_w, &o.opt_h, &o.max_w, &o.max_h, &o.min_w, &o.min_h, &o.sharp);
		o.ok = NVSDK_NGX_SUCCEED(r);
		logf("NGX optimal settings %ux%u mode %d (PerfQuality %d): %s 0x%08x | optimal %ux%u, dynamic min %ux%u max %ux%u, sharpness %.2f (unused: DLSS 310 has no sharpening)",
			ow, oh, mode, q, o.ok ? "ok" : "FAILED", r, o.opt_w, o.opt_h, o.min_w, o.min_h, o.max_w, o.max_h, o.sharp);
		g_optimal[key] = o;
		return o;
	}

	// Which render sub-rects a feature created at cw x ch (mode m) takes.
	// Evidence (research/evidence_user_session_1817/tf2-dlss-sr.log, every 0.1 feature created at InWidth = 1920x1200):
	//   sub-rect 1280x800 (Q), 1114x696 (B), 960x600 (P) evaluate ok; 640x400 (UP) fails EVERY evaluate with
	//   0xbad00005 (FAIL_InvalidParameter), 18:20:04.652 onwards, 1600/1600 in 3 windows.
	// So a sub-rect of exactly half the creation size works, a third does not. 0.2 therefore accepts
	// [ceil(c/2), c] per axis, narrowed by NGX's dynamic minimum when that is sane; and a rejected evaluate re-creates
	// the feature at exactly the sub-rect size (force_exact) before giving up on that size (g_rejected).
	void ngx_range(int mode, uint32_t ow, uint32_t oh, uint32_t cw, uint32_t ch, uint32_t &lo_w, uint32_t &lo_h)
	{
		lo_w = (cw + 1) / 2; lo_h = (ch + 1) / 2;
		const optimal_t o = query_optimal(ow, oh, mode);
		if (o.ok && o.min_w && o.min_h && o.min_w <= o.opt_w && o.min_h <= o.opt_h)
		{
			lo_w = lo_w > o.min_w ? lo_w : o.min_w;
			lo_h = lo_h > o.min_h ? lo_h : o.min_h;
		}
	}
	bool ngx_covers(uint32_t rw, uint32_t rh)
	{
		return g_ngx.feature && rw <= g_ngx.cw && rh <= g_ngx.ch && rw >= g_ngx.lo_w && rh >= g_ngx.lo_h;
	}

	bool ngx_init(ID3D11Device *dev)
	{
		if (g_ngx.inited || g_ngx.failed) return g_ngx.inited;
		// 0.4.1 / 0.3.2: nvngx_dlss.dll search = this add-on's folder first (owner: bin\x64_retail; Thunderstore package:
		// plugins\lib next to the add-ons), then the game's bin\x64_retail (the folder of engine.dll) when that differs.
		wchar_t module_dir[MAX_PATH] = {}, bin_dir[MAX_PATH] = {};
		wcsncpy_s(module_dir, g_dir.c_str(), _TRUNCATE);
		if (HMODULE eng = GetModuleHandleW(L"engine.dll"))
		{
			wchar_t ep[MAX_PATH] = {};
			const DWORD n = GetModuleFileNameW(eng, ep, MAX_PATH);
			if (n > 0 && n < MAX_PATH) wcsncpy_s(bin_dir, std::filesystem::path(ep).parent_path().c_str(), _TRUNCATE);
		}
		const wchar_t *paths[2] = { module_dir, bin_dir };
		const unsigned n_paths = (bin_dir[0] != 0 && _wcsicmp(bin_dir, module_dir) != 0) ? 2u : 1u;
		{
			const bool here = GetFileAttributesW((std::wstring(module_dir) + L"\\nvngx_dlss.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
			const bool there = n_paths == 2 && GetFileAttributesW((std::wstring(bin_dir) + L"\\nvngx_dlss.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
			logf("NGX: nvngx_dlss.dll search: add-on folder (%s)%s", here ? "present" : "absent", n_paths == 2 ? (there ? ", then bin\\x64_retail (present)" : ", then bin\\x64_retail (absent)") : " (= bin\\x64_retail)");
		}
		NVSDK_NGX_FeatureCommonInfo fci = {};
		fci.PathListInfo.Path = paths;
		fci.PathListInfo.Length = n_paths;
		wchar_t data_dir[MAX_PATH] = {};
		ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\tf2-dlss-sr", data_dir, MAX_PATH);
		CreateDirectoryW(data_dir, nullptr);
		NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID("3f9b6d2a-51c4-4e7b-9a0d-6c2e8f14b7a3", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "0.1",
		                                                         data_dir, dev, &fci, NVSDK_NGX_Version_API);
		if (NVSDK_NGX_FAILED(r)) { logf("NGX D3D11 init FAILED 0x%08x", r); g_ngx.failed = true; return false; }
		r = NVSDK_NGX_D3D11_GetCapabilityParameters(&g_ngx.params);
		if (NVSDK_NGX_FAILED(r)) { logf("NGX GetCapabilityParameters FAILED 0x%08x", r); g_ngx.failed = true; return false; }
		int available = 0, needs_driver = 0;
		unsigned int min_major = 0, min_minor = 0;
		NVSDK_NGX_Parameter_GetI(g_ngx.params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
		NVSDK_NGX_Parameter_GetI(g_ngx.params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
		NVSDK_NGX_Parameter_GetUI(g_ngx.params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &min_major);
		NVSDK_NGX_Parameter_GetUI(g_ngx.params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &min_minor);
		logf("NGX D3D11 init ok: DLSS SR available=%d needs_driver=%d (min %u.%u)", available, needs_driver, min_major, min_minor);
		if (!available) { g_ngx.failed = true; return false; }
		return g_ngx.inited = true;
	}

	int feature_flags()
	{
		int flags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AlphaUpscaling;
		if (g_cfg.depth_inverted) flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
		return flags;
	}
	// Does the current feature serve (mode, output, this render size) as it is?
	bool ngx_feature_ok(uint32_t ow, uint32_t oh, int mode, uint32_t rw, uint32_t rh)
	{
		return g_ngx.feature && !g_ngx.force_exact && g_ngx.ow == ow && g_ngx.oh == oh && g_ngx.mode == mode &&
		       g_ngx.flags == feature_flags() && g_ngx.preset == preset_for_mode(mode) && ngx_covers(rw, rh);
	}

	// (Re)create the feature so that it takes render sub-rect rw x rh for this mode. Creation size c:
	//   normal:      c = NGX's dynamic maximum for the mode (NGX_DLSS_GET_OPTIMAL_SETTINGS) -> Q/B/P/DLAA 1920x1200,
	//                UP 640x400 (NGX reports UP min = max = 640x400; a 1280x800 UP feature rejected the 640x400
	//                sub-rect with 0xbad00005, and the release/re-create after that rejection stalled the render
	//                thread for ~10 s, sr_a1_modes 19:16:52-19:17:03). Without a sane NGX answer:
	//                c = min(output, 2 x the mode's engine target) (0.2's rule)
	//   if rw x rh is not inside that: c = min(output, 2 x rw) (e.g. a 1920x1200 frame while in UP)
	//   force_exact: c = rw x rh (after NGX rejected rw x rh from a larger feature)
	bool ngx_ensure_feature(ID3D11DeviceContext *ctx, uint32_t ow, uint32_t oh, int mode, uint32_t rw, uint32_t rh)
	{
		if (ngx_feature_ok(ow, oh, mode, rw, rh)) return true;
		const int q = tf2nr::sr_ngx_perf_quality(static_cast<tf2nr::sr_mode>(mode));
		const int flags = feature_flags();
		const int preset = preset_for_mode(mode);
		uint32_t cw, ch, lo_w, lo_h;
		if (g_ngx.force_exact) { cw = rw; ch = rh; }
		else
		{
			uint32_t tw, th; mode_target(mode, ow, oh, tw, th);
			cw = 2 * tw < ow ? 2 * tw : ow; ch = 2 * th < oh ? 2 * th : oh;
			const optimal_t o = query_optimal(ow, oh, mode);
			if (o.ok && o.max_w && o.max_h && o.max_w <= ow && o.max_h <= oh && o.min_w <= o.max_w && o.min_h <= o.max_h)
			{
				cw = o.max_w; ch = o.max_h;
			}
			ngx_range(mode, ow, oh, cw, ch, lo_w, lo_h);
			if (!(rw <= cw && rh <= ch && rw >= lo_w && rh >= lo_h)) { cw = 2 * rw < ow ? 2 * rw : ow; ch = 2 * rh < oh ? 2 * rh : oh; }
		}
		if (cw < rw) cw = rw;
		if (ch < rh) ch = rh;
		ngx_range(mode, ow, oh, cw, ch, lo_w, lo_h);
		if (g_ngx.force_exact || rw < lo_w || rh < lo_h) { lo_w = rw < lo_w ? rw : lo_w; lo_h = rh < lo_h ? rh : lo_h; }

		if (g_ngx.feature) { NVSDK_NGX_D3D11_ReleaseFeature(g_ngx.feature); g_ngx.feature = nullptr; }
		// Presets: NVIDIA's defaults per quality mode (NGX 310 nvsdk_ngx_defs.h: K = default for DLAA/Balanced/Quality,
		// M = default for Performance, L = default for Ultra Performance; Streamline sets the same). Only the hint
		// matching this feature's PerfQuality matters; all five are written so a stale hint never leaks in.
		const struct { const char *key; int mode; } hints[] = {
			{ NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, 1 }, { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, 2 },
			{ NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, 3 }, { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, 4 },
			{ NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, 5 } };
		for (const auto &h : hints)
			NVSDK_NGX_Parameter_SetUI(g_ngx.params, h.key, static_cast<unsigned int>(preset_for_mode(h.mode)));
		NVSDK_NGX_DLSS_Create_Params cp = {};
		cp.Feature.InWidth = cw; cp.Feature.InHeight = ch;
		cp.Feature.InTargetWidth = ow; cp.Feature.InTargetHeight = oh;
		cp.Feature.InPerfQualityValue = static_cast<NVSDK_NGX_PerfQuality_Value>(q);
		cp.InFeatureCreateFlags = flags;
		cp.InEnableOutputSubrects = false;
		const NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(ctx, &g_ngx.feature, g_ngx.params, &cp);
		++g_st.recreates;
		logf("DLSS feature create in %ux%u -> out %ux%u mode %d (PerfQuality %d, preset %d, flags 0x%x) for render %ux%u, takes %ux%u..%ux%u%s: %s 0x%08x",
			cw, ch, ow, oh, mode, q, preset, flags, rw, rh, lo_w, lo_h, cw, ch, g_ngx.force_exact ? " [exact, after a rejected evaluate]" : "",
			NVSDK_NGX_SUCCEED(r) ? "ok" : "FAILED", r);
		const bool was_exact = g_ngx.force_exact;
		g_ngx.force_exact = false;
		if (NVSDK_NGX_FAILED(r))
		{
			g_ngx.feature = nullptr; g_ngx.mode = -1;
			if (was_exact) g_rejected.insert(rej_key(mode, rw, rh));
			return false;
		}
		g_ngx.ow = ow; g_ngx.oh = oh; g_ngx.quality = q; g_ngx.flags = flags; g_ngx.preset = preset; g_ngx.mode = mode;
		g_ngx.cw = cw; g_ngx.ch = ch; g_ngx.lo_w = lo_w; g_ngx.lo_h = lo_h;
		g_ngx.need_reset = true;
		return true;
	}

	// ------------------------------------------------------------------ the TSAA replacement
	// Returns true when DLSS produced this frame's image (the FG export snapshot was updated).
	bool run_dlss(command_list *cmd, const tsaa_cb &tcb, int mode)
	{
		auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native());
		if (!ctx || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) { ++g_st.skipped_deferred; return false; }
		device *dev = cmd->get_device();
		if (!g_gpu.dev) g_gpu.dev = reinterpret_cast<ID3D11Device *>(dev->get_native());
		if (!ngx_init(g_gpu.dev)) return false;

		const resource scene = g_fs.ps_srv[0], vel = g_fs.ps_srv[4], zbuf = g_fs.ps_srv[5], expo = g_fs.ps_srv[30];
		if (!scene.handle || !vel.handle || !zbuf.handle || !g_fs.rt0.handle) { ++g_st.skipped_inputs; return false; }
		const auto out_desc = dev->get_resource_desc(g_fs.rt0);
		const uint32_t ow = out_desc.texture.width, oh = out_desc.texture.height;
		if (!tcb.valid) { ++g_st.skipped_nojitter; return false; }
		const auto scene_desc = dev->get_resource_desc(scene);
		const uint32_t sw = scene_desc.texture.width, sh = scene_desc.texture.height;
		uint32_t rw = 0, rh = 0;
		bool size_from_tsaa = false;
		if (!resolve_render_size(tcb, sw, sh, ow, oh, rw, rh, size_from_tsaa)) { ++g_st.skipped_unknown; return false; }
		// diagnostic (0.3): 0.2's rule was "the LAST viewport bound in the main pass"; log the frames where it differs
		if (g_fs.render_seen_frame == g_fs.frames && (g_fs.render_w != rw || g_fs.render_h != rh))
		{
			++g_st.size_legacy_diff;
			static uint64_t s_logged = 0, s_last = 0;
			if (s_logged < 20 || g_fs.frames >= s_last + 600)
			{
				logf("render size: frame %llu uses %ux%u (TSAA %s); 0.2's rule (last main-pass viewport) would have fed DLSS %ux%u",
					g_fs.frames, rw, rh, size_from_tsaa ? "overrode the dominant viewport" : "agrees with the dominant viewport", g_fs.render_w, g_fs.render_h);
				++s_logged; s_last = g_fs.frames;
			}
		}
		g_obs_rw = rw; g_obs_ow = ow;
		if (rw < g_st.obs_min) g_st.obs_min = rw;
		if (rw > g_st.obs_max) g_st.obs_max = rw;

		// 0.1 evaluated only when the render size was within 4 px of the mode's scale, so every DVS ramp (seconds
		// after each mode switch / menu) and every frame at another size was skipped. 0.2 takes whatever size the
		// engine rendered this frame, as long as it is stable (renodx rebuilds NR per input size) and the feature
		// takes it; switching inputs/feature to a new size is rate-limited.
		static uint32_t last_w = 0, last_h = 0; static int stable = 0, last_mode = -1; static uint64_t next_change = 0;
		// A mode change also restarts the settle count: the new scale reaches the engine one frame after eng::update
		// wrote it, so without this the first frame (still at the old size) would build the new feature for the
		// old size and the real new size would then wait out the cooldown.
		if (mode != last_mode) g_rejected.clear();   // a new mode gets a fresh chance at every size (review P2)
		if (rw != last_w || rh != last_h || mode != last_mode) { last_w = rw; last_h = rh; last_mode = mode; stable = 0; }
		if (stable < 1000000) ++stable;
		if (g_rejected.count(rej_key(mode, rw, rh))) { ++g_st.skipped_rejected; return false; }
		const bool inputs_ok = targets_match(rw, rh, ow, oh);
		const bool feature_ok = ngx_feature_ok(ow, oh, mode, rw, rh);
		if (!inputs_ok || !feature_ok)
		{
			if (stable < g_cfg.settle_frames) { ++g_st.skipped_settle; return false; }
			// At most one input reallocation / feature re-creation per RecreateCooldown frames (mode changes included:
			// scrolling through the menu row must not rebuild renodx's NR working set every few frames - cf. the
			// NRLinearUnitNits=203 re-create storm that TDR'd the GPU). The exact-size retry after a rejected evaluate
			// is exempt (it happens once per size, then the size is either accepted or listed in g_rejected).
			if (!g_ngx.force_exact && g_fs.frames < next_change) { ++g_st.skipped_cooldown; return false; }
			next_change = g_fs.frames + static_cast<uint64_t>(g_cfg.recreate_cooldown);
			if (!ensure_targets(rw, rh, ow, oh) || !ngx_ensure_feature(ctx, ow, oh, mode, rw, rh)) { ++g_st.evals_fail; return false; }
		}

		if (!g_gpu.cs)
		{
			g_gpu.dev->CreateComputeShader(g_prep_cs, sizeof(g_prep_cs), nullptr, &g_gpu.cs);
			D3D11_BUFFER_DESC bd = { 32, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
			g_gpu.dev->CreateBuffer(&bd, nullptr, &g_gpu.params);
		}
		if (!g_gpu.cs || !g_gpu.params) { ++g_st.evals_fail; return false; }

		struct { uint32_t w, h; float jx, jy; float vel_scale, sub_jitter, pad[2]; } p =
			{ rw, rh, tcb.jx, tcb.jy, 1.0f / 1024.0f, g_cfg.mv_jittered ? 1.0f : 0.0f, { 0, 0 } };

		// Save the CS state we touch (NGX restores the immediate-context state it changes itself).
		ID3D11ComputeShader *old_cs = nullptr; ID3D11ClassInstance *old_ci[256] = {}; UINT old_nci = 256;
		ctx->CSGetShader(&old_cs, old_ci, &old_nci);
		ID3D11ShaderResourceView *old_srv[3] = {}; ID3D11UnorderedAccessView *old_uav[3] = {}; ID3D11Buffer *old_cb = nullptr;
		ctx->CSGetShaderResources(0, 3, old_srv); ctx->CSGetUnorderedAccessViews(0, 3, old_uav); ctx->CSGetConstantBuffers(0, 1, &old_cb);

		D3D11_MAPPED_SUBRESOURCE ms;
		if (SUCCEEDED(ctx->Map(g_gpu.params, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) { memcpy(ms.pData, &p, sizeof(p)); ctx->Unmap(g_gpu.params, 0); }

		auto game_view = [](uint32_t slot) { return reinterpret_cast<ID3D11ShaderResourceView *>(g_fs.ps_srv_view[slot].handle); };
		ID3D11ShaderResourceView *srvs[3] = { game_view(0) ? game_view(0) : typed_srv(scene, DXGI_FORMAT_R16G16B16A16_FLOAT),
		                                      game_view(4) ? game_view(4) : typed_srv(vel, DXGI_FORMAT_R16G16_FLOAT),
		                                      game_view(5) ? game_view(5) : typed_srv(zbuf, DXGI_FORMAT_R24_UNORM_X8_TYPELESS) };
		ID3D11UnorderedAccessView *uavs[3] = { g_gpu.color_uav, g_gpu.mv_uav, g_gpu.depth_uav };
		ctx->CSSetShader(g_gpu.cs, nullptr, 0);
		ctx->CSSetConstantBuffers(0, 1, &g_gpu.params);
		ctx->CSSetShaderResources(0, 3, srvs);
		ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
		ctx->Dispatch((rw + 7) / 8, (rh + 7) / 8, 1);
		ID3D11UnorderedAccessView *null_uav[3] = {}; ctx->CSSetUnorderedAccessViews(0, 3, null_uav, nullptr);
		ID3D11ShaderResourceView *null_srv[3] = {}; ctx->CSSetShaderResources(0, 3, null_srv);

		ctx->CSSetShader(old_cs, old_ci, old_nci);
		ctx->CSSetShaderResources(0, 3, old_srv); ctx->CSSetUnorderedAccessViews(0, 3, old_uav, nullptr); ctx->CSSetConstantBuffers(0, 1, &old_cb);
		if (old_cs) old_cs->Release();
		for (UINT i = 0; i < old_nci && i < 256; ++i) if (old_ci[i]) old_ci[i]->Release();
		for (auto *v : old_srv) if (v) v->Release();
		for (auto *v : old_uav) if (v) v->Release();
		if (old_cb) old_cb->Release();

		// TF2's t30 is the multiplier engine_post applies before its filmic curve = DLSS's exposure (guide 3.9). The TSAA
		// shader uses CBufCommonPerCamera c_forceExposure instead whenever it is > 0 (cb2[32].w), so do the same. Never
		// hand DLSS a null exposure: renodx builds its NR workset from the first evaluate's feed, and a missing exposure
		// made it build a v1 "frame meter" workset and then a second v2 one after every (re)create (research 2026-09-28).
		// Fallback order: forced exposure, t30, the last good value, 1.0.
		static bool s_expo_valid = false;
		static float s_expo_forced_last = -1.0f;
		static ID3D11Texture2D *s_expo_tex = nullptr;
		if (s_expo_tex != g_gpu.exposure) { s_expo_tex = g_gpu.exposure; s_expo_valid = false; s_expo_forced_last = -1.0f; }   // new device/texture: contents undefined
		const auto half_bits = [](float f) -> uint16_t {
			uint32_t x; memcpy(&x, &f, 4);
			const uint32_t sign = (x >> 16) & 0x8000u; int e = static_cast<int>((x >> 23) & 0xff) - 127 + 15; uint32_t m = x & 0x7fffffu;
			if (e <= 0) return static_cast<uint16_t>(sign); if (e >= 31) return static_cast<uint16_t>(sign | 0x7c00u);
			return static_cast<uint16_t>(sign | (static_cast<uint32_t>(e) << 10) | (m >> 13)); };
		if (tcb.force_exposure > 0.0f)
		{
			if (tcb.force_exposure != s_expo_forced_last || !s_expo_valid)
			{
				const uint16_t h = half_bits(tcb.force_exposure);
				ctx->UpdateSubresource(g_gpu.exposure, 0, nullptr, &h, 2, 2);
				s_expo_forced_last = tcb.force_exposure;
			}
			s_expo_valid = true;
		}
		else
		{
			s_expo_forced_last = -1.0f;
			bool copied = false;
			if (expo.handle)
			{
				const auto ed = dev->get_resource_desc(expo);
				if (ed.texture.width == 1 && ed.texture.height == 1 && (ed.texture.format == format::r16_typeless || ed.texture.format == format::r16_float))
				{
					ctx->CopyResource(g_gpu.exposure, reinterpret_cast<ID3D11Resource *>(expo.handle));
					copied = true;
				}
			}
			if (!copied && !s_expo_valid)
			{
				const uint16_t one = half_bits(1.0f);
				ctx->UpdateSubresource(g_gpu.exposure, 0, nullptr, &one, 2, 2);
			}
			s_expo_valid = true;
		}
		ID3D11Resource *exposure_tex = g_gpu.exposure;

		const bool cut = tcb.blend >= 0.999f || (g_fs.frames - g_fs.last_tsaa_frame) > 2;   // TSAA history reset or a menu gap
		NVSDK_NGX_D3D11_DLSS_Eval_Params ep = {};
		ep.Feature.pInColor = g_gpu.color;
		ep.Feature.pInOutput = g_gpu.out;
		ep.pInDepth = g_gpu.depth;
		ep.pInMotionVectors = g_gpu.mv;
		ep.InJitterOffsetX = g_cfg.jsx * tcb.jx * static_cast<float>(rw);
		ep.InJitterOffsetY = g_cfg.jsy * tcb.jy * static_cast<float>(rh);
		ep.InRenderSubrectDimensions = { rw, rh };
		ep.InReset = (g_ngx.need_reset || cut) ? 1 : 0;
		ep.InMVScaleX = 1.0f; ep.InMVScaleY = 1.0f;
		ep.pInExposureTexture = exposure_tex;
		ep.InPreExposure = 1.0f; ep.InExposureScale = 1.0f;
		bool evaluated = false;
		// tf2-nr-linux: NR placement (see refresh_nrl). DLSS output and inputs have one size only in DLAA.
		const bool nrl_on = g_nrl_route && nr_menu_on();
		const bool nrl_post = nrl_on && g_cfg.nr_order == 0 && rw == ow && rh == oh;
		const auto nrl_run = [&](ID3D11Texture2D *color, const char *where) {
			tf2nr_linux_frame f = {};
			f.version = TF2NR_LINUX_FRAME_VERSION;
			f.device = g_gpu.dev; f.context = ctx;
			f.color = color; f.depth = g_gpu.depth; f.motion = g_gpu.mv; f.exposure = g_gpu.exposure;
			f.width = rw; f.height = rh;
			f.mv_scale_x = 1.0f; f.mv_scale_y = 1.0f;   // prep_cs writes render pixels, current + mv = previous
			f.reset = ep.InReset;
			depth_guard nested;   // events our add-on's commands might raise on this thread are skipped
			if (g_nrl_apply(&f)) ++g_nrl_ok;
			else if (g_nrl_fail++ % 600 == 0) logf("tf2-nr-linux: NR not applied %s DLSS (%llu times; see tf2-nr-linux.log)", where, static_cast<unsigned long long>(g_nrl_fail));
		};
		if (nrl_on && !nrl_post) nrl_run(g_gpu.color, "before");
		const NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, g_ngx.feature, g_ngx.params, &ep);
		g_st.last_ngx_result = static_cast<int>(r);
		if (NVSDK_NGX_SUCCEED(r) && nrl_post) nrl_run(g_gpu.out, "after");
		if (NVSDK_NGX_SUCCEED(r))
		{
			g_ngx.need_reset = false;
			g_fs.overwrite_dst = g_fs.rt0;   // lands at the next GPU command, after the TSAA draw executed
			++g_st.evals_ok;
			g_last_eval_frame = g_fs.frames;
			evaluated = true;
			// FG export: this frame's DLSS inputs (prep_cs outputs) are now queued; publish them.
			uint32_t trw = 0, trh = 0;
			tsaa_size(tcb, sw, sh, trw, trh);
			std::lock_guard lk(fgx::mx);
			fgx::hold(fgx::s.depth, g_gpu.depth); fgx::hold(fgx::s.mv, g_gpu.mv); fgx::hold(fgx::s.expo, g_gpu.exposure);
			fgx::s.valid = g_gpu.depth && g_gpu.mv && g_gpu.exposure;
			fgx::s.frame = g_fs.frames; fgx::s.serial = ++fgx::serial; fgx::s.generation = fgx::generation;
			fgx::s.rw = rw; fgx::s.rh = rh; fgx::s.ow = ow; fgx::s.oh = oh; fgx::s.trw = trw; fgx::s.trh = trh; fgx::s.mode = mode;
			fgx::s.jx_px = ep.InJitterOffsetX; fgx::s.jy_px = ep.InJitterOffsetY; fgx::s.jx_uv = tcb.jx; fgx::s.jy_uv = tcb.jy;
			fgx::s.expo_forced = tcb.force_exposure > 0.0f ? tcb.force_exposure : 0.0f;
			fgx::s.flags = (ep.InReset ? TF2SR_FG_RESET : 0u) | (g_cfg.mv_jittered ? TF2SR_FG_MV_JITTER_FREE : 0u) |
			               (g_cfg.depth_inverted ? TF2SR_FG_DEPTH_INVERTED : 0u) | ((rw < ow || rh < oh) ? TF2SR_FG_UPSCALING : 0u) |
			               (tcb.force_exposure > 0.0f ? TF2SR_FG_EXPOSURE_FORCED : 0u) | (size_from_tsaa ? TF2SR_FG_SIZE_FROM_TSAA : 0u);
			// camera: this frame's main-view snapshot, and only when the main view was drawn at the size DLSS gets
			const cam::result_t &cr = cam::frame_res;
			const bool cam_fresh = g_cfg.camera_readback && cr.valid && cr.frame == g_fs.frames;
			const bool cam_ok = cam_fresh && std::fabs(cr.rt_w * cr.vps_x - static_cast<float>(rw)) <= 2.0f &&
			                    std::fabs(cr.rt_h * cr.vps_y - static_cast<float>(rh)) <= 2.0f;
			if (cam_fresh && !cam_ok) ++cam::size_mismatch;
			if (cam_ok) ++cam::published;
			fgx::s.cam_valid = cam_ok; fgx::s.cam_far_inf = cam_ok && cr.far_inf;
			fgx::s.cam_near = cam_ok ? cr.znear : 0.0f; fgx::s.cam_far = cam_ok ? cr.zfar : 0.0f;
			fgx::s.cam_fovy = cam_ok ? cr.fovy : 0.0f; fgx::s.cam_aspect = cam_ok ? cr.aspect : 0.0f;
			fgx::s.camera_frame = cam_ok ? cr.frame : 0;
			// 0.3.1: camera-cut hint of this frame's main view vs the previous frame's (a property of the camera itself, so
			// it does not need the render-size agreement above)
			if (cam_fresh && cr.cut) fgx::s.flags |= TF2SR_FG_CAMERA_CUT;
		}
		else
		{
			// Nothing is copied on failure: the player sees TF2's own TSAA frame for this frame (valid image, just
			// the engine's upscale; its history is our previous DLSS output).
			if (g_st.evals_fail < 5) logf("DLSS evaluate FAILED 0x%08x (render %ux%u -> %ux%u, feature in %ux%u mode %d)", r, rw, rh, ow, oh, g_ngx.cw, g_ngx.ch, mode);
			++g_st.evals_fail;
			if (r == NVSDK_NGX_Result_FAIL_InvalidParameter)
			{
				if (g_ngx.cw != rw || g_ngx.ch != rh)
				{
					g_ngx.force_exact = true;   // next frame: re-create at exactly rw x rh (no cooldown)
					logf("DLSS rejected sub-rect %ux%u from a feature created at %ux%u (mode %d) -> re-creating at exactly %ux%u", rw, rh, g_ngx.cw, g_ngx.ch, mode, rw, rh);
				}
				else
				{
					// Only a run of exact-size rejections lists the size: one transient failure (DLAA's feature is
					// always exact-size) must not switch DLSS off for the session (review P2). Cleared on mode change.
					static uint64_t s_fail_key = 0; static int s_fail_run = 0;
					const uint64_t k = rej_key(mode, rw, rh);
					s_fail_run = (k == s_fail_key) ? s_fail_run + 1 : 1; s_fail_key = k;
					if (s_fail_run >= 30)
					{
						g_rejected.insert(k); s_fail_run = 0;
						logf("DLSS rejected render %ux%u -> %ux%u in mode %d 30 times in a row even with a feature created at that size - skipped until the mode changes (TSAA frame shown)",
							rw, rh, ow, oh, mode);
					}
				}
			}
		}
		g_fs.last_tsaa_frame = g_fs.frames;
		if (rw < g_st.rw_min) g_st.rw_min = rw;
		if (rw > g_st.rw_max) g_st.rw_max = rw;
		return evaluated;
	}

	// ------------------------------------------------------------------ texture LOD bias for upscaled modes
	// D3D11 sampler states are immutable and TF2 creates its samplers at load, so the bias cannot go in at creation.
	// Instead, while the main scene pass (RGBA16 + RG16 MRT) draws, each game sampler bound to a PS slot that
	// filters with a linear/anisotropic minification is replaced natively (ID3D11DeviceContext::PSSetSamplers on
	// the real context, not through ReShade -> no events) by a cached twin = the same desc with MipLODBias + bias.
	// The original goes back as soon as the main pass ends (Source's state cache would otherwise keep our twin for
	// later passes that think the original is still bound) and whenever the game rebinds the slot. Point, comparison
	// (shadow) and min/max samplers, UI, post and the TSAA/engine_post draws are never touched. Twins are keyed by
	// (sampler, bias in 1/8 steps), so a mode change just selects other twins.
	namespace mip
	{
		std::unordered_map<uint64_t, ID3D11SamplerState *> twins;   // key: game sampler | (bias_q & 0xff) << 56
		std::unordered_map<uint64_t, bool> qualifies;               // game sampler -> gets a twin
		int bias_q = 0;                                             // target bias, 1/8 steps (0 = off)

		uint64_t key(uint64_t s, int q) { return s ^ (static_cast<uint64_t>(q & 0xff) << 56); }

		ID3D11SamplerState *twin(ID3D11Device *dev, uint64_t s, int q)
		{
			if (!dev || !s) return nullptr;
			auto qi = qualifies.find(s);
			D3D11_SAMPLER_DESC d = {};
			reinterpret_cast<ID3D11SamplerState *>(s)->GetDesc(&d);
			if (qi == qualifies.end())
			{
				const UINT f = static_cast<UINT>(d.Filter);
				const bool ok = (f & 0x180u) == 0 &&                  // not comparison (0x80) / minimum (0x100) / maximum (0x180)
				                ((f & 0x40u) != 0 || (f & 0x10u) != 0) && // anisotropic or linear minification
				                d.MaxLOD > d.MinLOD;                      // has a mip range to bias
				qi = qualifies.emplace(s, ok).first;
			}
			if (!qi->second) return nullptr;
			const uint64_t k = key(s, q);
			auto it = twins.find(k);
			if (it != twins.end()) return it->second;
			float b = d.MipLODBias + q / 8.0f;
			d.MipLODBias = b < -16.0f ? -16.0f : (b > 15.99f ? 15.99f : b);
			ID3D11SamplerState *t = nullptr;
			if (FAILED(dev->CreateSamplerState(&d, &t))) t = nullptr;
			twins[k] = t;   // cache failures too (nullptr) so we do not retry every draw
			return t;
		}

		void restore(ID3D11DeviceContext *ctx)
		{
			g_fs.samp_checked_mask = 0;
			if (!ctx || !g_fs.samp_twin_mask) { g_fs.samp_twin_mask = 0; return; }
			for (UINT slot = 0; slot < 16; ++slot)
			{
				if ((g_fs.samp_twin_mask & (1u << slot)) == 0) continue;
				ID3D11SamplerState *orig = reinterpret_cast<ID3D11SamplerState *>(g_fs.ps_samp[slot]);
				ctx->PSSetSamplers(slot, 1, &orig);
			}
			g_fs.samp_twin_mask = 0;
		}

		void apply(command_list *cmd)
		{
			auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native());
			if (!ctx || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
			if (bias_q == 0 || g_fs.samp_twin_bias_q != bias_q) { restore(ctx); g_fs.samp_twin_bias_q = bias_q; }
			if (bias_q == 0) return;
			if (!g_gpu.dev) g_gpu.dev = reinterpret_cast<ID3D11Device *>(cmd->get_device()->get_native());
			if (g_fs.samp_checked_mask == 0xFFFFu) return;
			for (UINT slot = 0; slot < 16; ++slot)
			{
				if (g_fs.samp_checked_mask & (1u << slot)) continue;
				g_fs.samp_checked_mask |= 1u << slot;
				const uint64_t s = g_fs.ps_samp[slot];
				if (!s) continue;
				ID3D11SamplerState *t = twin(g_gpu.dev, s, bias_q);
				if (!t) continue;
				ctx->PSSetSamplers(slot, 1, &t);
				g_fs.samp_twin_mask |= 1u << slot;
				++g_st.samp_twins_bound;
			}
		}

		// once per frame from present
		void update(int mode)
		{
			int q = 0;
			if (g_cfg.mip_bias && mode >= 2 && g_obs_rw && g_obs_ow && g_obs_rw < g_obs_ow && g_fs.frames < g_last_eval_frame + 120)
			{
				float b = std::log2(static_cast<float>(g_obs_rw) / static_cast<float>(g_obs_ow)) + g_cfg.mip_bias_offset;
				b = b < -3.0f ? -3.0f : (b > 0.0f ? 0.0f : b);
				q = static_cast<int>(std::lround(b * 8.0f));
			}
			if (q != bias_q)
				logf("mip bias -> %.3f (mode %d, render width %u / display %u, offset %.2f)%s", q / 8.0f, mode, g_obs_rw, g_obs_ow, g_cfg.mip_bias_offset,
					q == 0 ? " = off (DLAA/off, MipBias=0 or DLSS not producing frames)" : "");
			bias_q = q;
		}

		void forget(uint64_t s)
		{
			qualifies.erase(s);
			for (auto it = twins.begin(); it != twins.end();)
			{
				if ((it->first & 0x00FFFFFFFFFFFFFFull) == (s & 0x00FFFFFFFFFFFFFFull)) { safe_release(it->second); it = twins.erase(it); }
				else ++it;
			}
			for (UINT slot = 0; slot < 16; ++slot)
				if (g_fs.ps_samp[slot] == s) { g_fs.ps_samp[slot] = 0; g_fs.samp_twin_mask &= ~(1u << slot); g_fs.samp_checked_mask &= ~(1u << slot); }
		}

		void release_all()
		{
			for (auto &kv : twins) safe_release(kv.second);
			twins.clear(); qualifies.clear();
			g_fs.samp_twin_mask = 0; g_fs.samp_checked_mask = 0;
		}
	}

	void on_destroy_sampler(device *dev, sampler s)
	{
		if (foreign_dev(dev)) return;
		SR_LOCK_ALWAYS();
		mip::forget(s.handle);
	}

	void flush_overwrite(command_list *cmd)
	{
		if (!g_fs.overwrite_dst.handle || !g_gpu.out) return;
		auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native());
		if (ctx && ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE)
			ctx->CopyResource(reinterpret_cast<ID3D11Resource *>(g_fs.overwrite_dst.handle), g_gpu.out);   // RGBA16 typeless <- RGBA16F
		g_fs.overwrite_dst = { 0 };
	}

	void on_bind_rts(command_list *cmd, uint32_t count, const resource_view *rtvs, resource_view dsv)
	{
		if (foreign_cmd(cmd) || (count > 0 && rtvs == nullptr)) return;
		device *dev = cmd->get_device();
		SR_LOCK();
		flush_overwrite(cmd);
		g_fs.rt_count = count;
		g_fs.rt0 = count > 0 && rtvs[0].handle ? dev->get_resource_from_view(rtvs[0]) : resource { 0 };
		g_fs.dsv_res = dsv.handle ? dev->get_resource_from_view(dsv) : resource { 0 };
		const bool was_main = g_fs.main_pass;
		g_fs.main_pass = false;
		if (count >= 2 && rtvs[0].handle && rtvs[1].handle)
		{
			const auto d0 = dev->get_resource_desc(g_fs.rt0);
			const auto d1 = dev->get_resource_desc(dev->get_resource_from_view(rtvs[1]));
			g_fs.main_pass = (d0.texture.format == format::r16g16b16a16_typeless || d0.texture.format == format::r16g16b16a16_float) &&
			                 (d1.texture.format == format::r16g16_typeless || d1.texture.format == format::r16g16_float) &&
			                 d0.texture.width == d1.texture.width && d0.texture.height == d1.texture.height;
		}
		// leaving the main scene pass: put the game's own samplers back before any other pass draws
		if (was_main && !g_fs.main_pass && g_fs.samp_twin_mask)
		{
			auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native());
			if (ctx && ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE) mip::restore(ctx);   // twins only ever go on the immediate context
		}
	}

	bool on_draw_common(command_list *cmd)
	{
		if (foreign_cmd(cmd)) return false;
		SR_LOCK(false);
		flush_overwrite(cmd);
		if (g_fs.ps_tags & TAG_EPOST) g_fs.epost_fb = g_fs.ps_srv[1];
		if ((g_fs.ps_tags & (TAG_TSAA | TAG_EPOST)) == 0)
		{
			if (g_fs.main_pass)
			{
				note_main_draw();   // render-size histogram (0.3)
				if (g_cfg.camera_readback && !cam::snapped) cam::snap_main();   // the main view's b2 upload (0.3) + cut hint (0.3.1)
				if (mip::bias_q != 0 || g_fs.samp_twin_mask) mip::apply(cmd);   // material draws only
			}
			return false;
		}
		if ((g_fs.ps_tags & TAG_TSAA) == 0) return false;
		if (g_fs.samp_twin_mask)
		{
			auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native());
			if (ctx && ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE) mip::restore(ctx);
		}

		++g_fs.tsaa_draws_this_frame;
		++g_st.tsaa_draws;
		if (g_fs.ps_cb1.buffer.handle && g_tsaa_cbs.insert(g_fs.ps_cb1.buffer.handle).second)
			logf("CBufTSAA buffer learned: %016llx (offset %llu size %llu)", g_fs.ps_cb1.buffer.handle, g_fs.ps_cb1.offset, g_fs.ps_cb1.size);
		if (g_fs.ps_cb2.buffer.handle && g_tsaa_cbs.insert(g_fs.ps_cb2.buffer.handle).second && g_tsaa_cbs.size() < 64)
			logf("CBufCommonPerCamera buffer learned (TSAA b2): %016llx (offset %llu size %llu)", g_fs.ps_cb2.buffer.handle, g_fs.ps_cb2.offset, g_fs.ps_cb2.size);
		// b2 is learned whatever CameraReadback says (0.3.1): its uploads also carry TSAA's c_forceExposure
		if (g_fs.ps_cb2.buffer.handle && g_fs.ps_cb2.buffer.handle != cam::b2.load(std::memory_order_relaxed))
		{
			const auto bd = cmd->get_device()->get_resource_desc(g_fs.ps_cb2.buffer);
			if (bd.type == resource_type::buffer && bd.buffer.size == 576)
			{
				cam::b2.store(g_fs.ps_cb2.buffer.handle); cam::have_latest = false;
				logf("camera: CBufCommonPerCamera %016llx (576 bytes) - uploads tracked (TSAA's c_forceExposure%s)", g_fs.ps_cb2.buffer.handle,
					g_cfg.camera_readback ? "; main view decoded at the first main-pass draw" : "");
			}
		}
		const tsaa_cb tcb = read_cbuf_tsaa();
		if (g_fs.tsaa_draws_this_frame == 1)
		{
			++g_fexp.frames;
			if (tcb.cam_age > 1) ++g_fexp.invalid;
			else
			{
				if (tcb.force_exposure > 0.0f) ++g_fexp.forced;
				g_fexp.lo = std::fmin(g_fexp.lo, tcb.force_exposure); g_fexp.hi = std::fmax(g_fexp.hi, tcb.force_exposure);
			}
		}
		if (tcb.valid)
		{
			g_st.jx_min = std::fmin(g_st.jx_min, tcb.jx); g_st.jx_max = std::fmax(g_st.jx_max, tcb.jx);
			g_st.jy_min = std::fmin(g_st.jy_min, tcb.jy); g_st.jy_max = std::fmax(g_st.jy_max, tcb.jy);
		}
		if (g_cfg.probe && g_probe_logs < 4 && tcb.age <= 1) { probe_log(cmd, tcb, g_probe_windows == 0 ? "first sightings" : "window sample"); ++g_probe_logs; }
		const int mode = current_mode();
		if (g_fs.tsaa_draws_this_frame == 1)
		{
			bool evaluated = false;
			if (g_cfg.evaluate && mode != 0)
			{
				try { evaluated = run_dlss(cmd, tcb, mode); }
				catch (const std::exception &e) { logf("run_dlss threw '%s' - DLSS SR disabled for this session", e.what()); g_cfg.evaluate = 0; }
				catch (...) { logf("run_dlss threw a non-std exception - DLSS SR disabled for this session"); g_cfg.evaluate = 0; }
			}
			if (!evaluated) fgx::invalidate();   // this frame has no DLSS inputs: the previous frame's must not look current
		}
		return false;   // let TSAA run: it also writes TF2's other MRTs
	}

	bool on_draw(command_list *cmd, uint32_t, uint32_t, uint32_t, uint32_t) { return on_draw_common(cmd); }
	bool on_draw_indexed(command_list *cmd, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { return on_draw_common(cmd); }
	bool on_dispatch(command_list *cmd, uint32_t, uint32_t, uint32_t) { if (foreign_cmd(cmd)) return false; SR_LOCK(false); flush_overwrite(cmd); return false; }
	bool on_indirect(command_list *cmd, indirect_command, resource, uint64_t, uint32_t, uint32_t) { if (foreign_cmd(cmd)) return false; SR_LOCK(false); flush_overwrite(cmd); return false; }
	bool on_copy_resource(command_list *cmd, resource, resource) { if (foreign_cmd(cmd)) return false; SR_LOCK(false); flush_overwrite(cmd); return false; }
	bool on_copy_texture_region(command_list *cmd, resource, uint32_t, const subresource_box *, resource, uint32_t, const subresource_box *, filter_mode)
	{ if (foreign_cmd(cmd)) return false; SR_LOCK(false); flush_overwrite(cmd); return false; }
	bool on_clear_rt(command_list *cmd, resource_view, const float[4], uint32_t, const rect *) { if (foreign_cmd(cmd)) return false; SR_LOCK(false); flush_overwrite(cmd); return false; }
	bool on_clear_ds(command_list *cmd, resource_view, const float *, const uint8_t *, uint32_t, const rect *) { if (foreign_cmd(cmd)) return false; SR_LOCK(false); flush_overwrite(cmd); return false; }

	bool g_sign_down = false, g_mv_down = false;
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

	// tf2-fg link, once per frame (cheap: GetModuleHandleW; GetProcAddress only when the module changes).
	void refresh_fg_link()
	{
		HMODULE m = GetModuleHandleW(L"tf2-fg.addon64");
		if (m == g_fg_mod) return;
		g_fg_mod = m;
		const auto f = m ? reinterpret_cast<PFN_tf2fg_in_native_present>(GetProcAddress(m, "tf2fg_in_native_present")) : nullptr;
		g_fg_native.store(f);
		logf("tf2-fg link: %s", m == nullptr ? "tf2-fg.addon64 not loaded" : (f ? "tf2fg_in_native_present resolved - per-draw work is skipped while tf2-fg's native Present runs" : "tf2-fg.addon64 has no tf2fg_in_native_present export"));
	}

	// no-TSAA guard (NoTsaaUnpin): an upscaling pin with no TSAA pass to replace = a blurry engine upscale for nothing.
	int g_no_tsaa_run = 0;
	bool g_unpinned = false;
	uint64_t g_unpin_count = 0, g_no_tsaa_logged = 0;

	void on_present(command_queue *queue, swapchain *, const rect *, const rect *, uint32_t, const rect *)
	{
		if (queue == nullptr) return;
		{
			// the game's device = the first D3D11 device that presents (0.3.1); every other device's events are ignored
			device *const qd = queue->get_device();
			if (qd != nullptr && g_game_dev.load(std::memory_order_relaxed) == nullptr && qd->get_api() == device_api::d3d11)
			{
				g_game_dev.store(qd);
				logf("game device %p (D3D11, first present) - events of any other device are ignored", static_cast<void *>(qd));
			}
			if (foreign_dev(qd)) return;
		}
		SR_LOCK();
		command_list *imm = queue->get_immediate_command_list();
		{
			// flush a pending overwrite that no later command picked up (should not happen: TSAA is followed by more passes)
			if (imm) flush_overwrite(imm);
		}
		refresh_fg_link();
		tf2nr::on_present(nullptr, &logf);
		tf2nr::tuning::update(&logf);   // DLSS 5 Adjustments sliders -> renodx (live), persisted to ReShade.ini   // reads the Video-menu convars (runtime nullptr: never touches effects)
		if ((g_fs.frames % 300) == 0) refresh_route();

		const int mode = current_mode();
		const bool had_tsaa = g_fs.tsaa_draws_this_frame != 0;
		if (had_tsaa)
		{
			g_no_tsaa_run = 0;
			if (g_unpinned) { g_unpinned = false; logf("no-TSAA guard: TSAA pass back at frame %llu -> render scale pinned again (mode %d)", g_fs.frames, mode); }
		}
		else if (main_draws_this_frame() && eng::applied && mode >= 2)
		{
			// the scene was drawn at the pinned scale, but no TSAA pass (= nothing for DLSS to replace) this frame
			++g_st.no_tsaa_pinned; ++g_no_tsaa_run;
			if (g_no_tsaa_logged < 10)
			{
				char vb[192]; vph_str(vb, sizeof(vb));
				logf("no-TSAA frame %llu while mode %d pins the render scale: main-pass viewports %s", g_fs.frames, mode, vb);
				++g_no_tsaa_logged;
			}
			if (g_cfg.no_tsaa_unpin > 0 && g_no_tsaa_run >= g_cfg.no_tsaa_unpin && !g_unpinned)
			{
				g_unpinned = true; ++g_unpin_count;
				logf("no-TSAA guard: %d frames in a row drew the scene without a TSAA pass (frame %llu) -> pin lifted until TSAA returns", g_no_tsaa_run, g_fs.frames);
			}
		}
		eng::update(g_unpinned ? 0 : mode, g_fs.frames);
		mip::update(mode);
		nrsync::update(g_fs.frames);

		// Frames whose main pass used more than one full-depth viewport size (the death cam does): logged, rate-limited.
		if (main_draws_this_frame() && g_vph.n > 1)
		{
			++g_st.multi_vp_frames;
			static uint64_t s_logged = 0, s_last = 0;
			if (s_logged < 20 || g_fs.frames >= s_last + 600)
			{
				char vb[192]; vph_str(vb, sizeof(vb));
				logf("main pass frame %llu used several viewport sizes: %s (TSAA %s)", g_fs.frames, vb, had_tsaa ? "yes" : "no");
				++s_logged; s_last = g_fs.frames;
			}
		}

		// FG export: a frame presented without its own DLSS inputs invalidates the snapshot.
		{
			std::lock_guard lk(fgx::mx);
			if (fgx::s.valid && fgx::s.frame == g_fs.frames) ++g_st.fgx_valid_frames;
			else fgx::s.valid = false;
		}
		cam::end_frame();

		if (had_tsaa) ++g_st.tsaa_frames;
		g_fs.tsaa_draws_this_frame = 0;
		++g_fs.frames;
		fgx::sr_frame.store(g_fs.frames);

		if (key_edge(g_cfg.sign_key, g_sign_down))
		{
			// (-,-) -> (+,-) -> (-,+) -> (+,+)
			if (g_cfg.jsx < 0) g_cfg.jsx = 1; else { g_cfg.jsx = -1; g_cfg.jsy = -g_cfg.jsy; }
			g_ngx.need_reset = true;
			logf("jitter sign -> %+.0f,%+.0f", g_cfg.jsx, g_cfg.jsy);
		}
		if (key_edge(g_cfg.mv_key, g_mv_down))
		{
			g_cfg.mv_jittered ^= 1; g_ngx.need_reset = true;
			logf("MVJittered -> %d", g_cfg.mv_jittered);
		}

		if ((g_fs.frames % 600) == 0)
		{
			const uint64_t skipped_size = g_st.skipped_settle + g_st.skipped_cooldown + g_st.skipped_unknown + g_st.skipped_rejected;
			logf("frame %llu: mode %d | TSAA frames %llu/600 (draws %llu) | evals ok %llu fail %llu (last 0x%08x) | skipped: no jitter %llu, deferred %llu, inputs %llu, size %llu | "
				"render width %u..%u | c_jitter x %.6f..%.6f y %.6f..%.6f | nested events skipped %llu",
				g_fs.frames, current_mode(), g_st.tsaa_frames, g_st.tsaa_draws, g_st.evals_ok, g_st.evals_fail, g_st.last_ngx_result,
				g_st.skipped_nojitter, g_st.skipped_deferred, g_st.skipped_inputs, skipped_size, g_st.rw_min == 0xFFFFFFFF ? 0 : g_st.rw_min, g_st.rw_max,
				g_st.jx_min, g_st.jx_max, g_st.jy_min, g_st.jy_max, g_nested);
			logf("   size detail: settle %llu, cooldown %llu, unknown %llu, rejected %llu | observed render width %u..%u (TSAA clamp + viewport agree %llu, TSAA clamp only %llu, "
				"TSAA clamp overrode the viewport %llu, viewport only %llu, b2 %llu; TSAA constants invalid %llu; frames with several main-pass viewport sizes %llu; frames where 0.2's last-viewport rule differs %llu) | "
				"feature re-creates %llu, input reallocs %llu (feature in %ux%u takes %ux%u..%ux%u, mode %d, preset %d) | mip bias %.3f, sampler twins bound %llu",
				g_st.skipped_settle, g_st.skipped_cooldown, g_st.skipped_unknown, g_st.skipped_rejected,
				g_st.obs_min == 0xFFFFFFFF ? 0 : g_st.obs_min, g_st.obs_max, g_st.size_agree, g_st.size_tsaa_only, g_st.size_tsaa_override, g_st.size_src_viewport,
				g_st.size_src_cbuf, g_st.size_tsaa_invalid, g_st.multi_vp_frames, g_st.size_legacy_diff, g_st.recreates, g_st.reallocs,
				g_ngx.cw, g_ngx.ch, g_ngx.lo_w, g_ngx.lo_h, g_ngx.cw, g_ngx.ch, g_ngx.mode, g_ngx.preset, mip::bias_q / 8.0f, g_st.samp_twins_bound);
			{
				fgx::snap_t s;
				{
					std::lock_guard lk(fgx::mx);
					s = fgx::s;
					s.depth = s.mv = s.expo = nullptr;   // copy of the fields only, no reference taken
				}
				const cam::result_t &c = cam::last;
				logf("   fg export: frames with valid inputs %llu/600 | calls %llu (valid %llu, refused %llu) | generation %llu serial %llu | last frame %llu: %ux%u -> %ux%u (TSAA %ux%u) mode %d flags 0x%03x jitter %.3f,%.3f px | "
					"camera %s near %.3f far %.1f%s fovY %.2f deg aspect %.4f (frame %llu) | no-TSAA scene frames while pinned %llu (lifts %llu, lifted now %d) | tf2-fg link %s, events skipped in its native Present %llu",
					g_st.fgx_valid_frames, fgx::calls.exchange(0), fgx::calls_valid.exchange(0), fgx::calls_bad.exchange(0), fgx::generation, fgx::serial,
					s.frame, s.rw, s.rh, s.ow, s.oh, s.trw, s.trh, s.mode, s.flags, s.jx_px, s.jy_px,
					s.cam_valid ? "valid" : "INVALID", s.cam_near, s.cam_far, s.cam_far_inf ? " (inf)" : "", s.cam_fovy * 57.29578f, s.cam_aspect, s.camera_frame,
					g_st.no_tsaa_pinned, g_unpin_count, g_unpinned ? 1 : 0, g_fg_native.load() ? "yes" : "no", g_fg_skips.exchange(0));
				logf("   camera (b2 uploads %llu): main-pass snapshots %llu, decoded ok %llu, rejected %llu (last reason: %s), no upload %llu, render-size mismatch at publish %llu, published %llu | last frame %llu: near %.4f (c_zNear %.4f / MaxZ %.3f) far %.1f%s k %.7f L3 %.5f C33 %.6f fovY %.3f deg aspect %.4f | c_renderTargetSize %.0fx%.0f x c_viewportScale %.4f,%.4f = %.1fx%.1f, c_framebufferViewportScale %.4f,%.4f, c_forceExposure %.4f",
					cam::updates, cam::snaps, cam::ok_count, cam::bad_count, cam::last_reason, cam::no_update, cam::size_mismatch, cam::published,
					c.frame == ~0ull ? 0ull : c.frame, c.znear, c.znear_cb, c.vp_maxz, c.zfar, c.far_inf ? " (inf)" : "", c.k, c.l3, c.c33, c.fovy * 57.29578f, c.aspect,
					c.rt_w, c.rt_h, c.vps_x, c.vps_y, c.rt_w * c.vps_x, c.rt_h * c.vps_y, c.fbvs_x, c.fbvs_y, c.force_exposure);
				logf("   camera-cut hint (FG): %llu cut frames of %llu compared | largest per-frame change without a cut: moved %.1f units (limit %.0f), turned %.2f deg (limit %.0f), fovY %.2f deg (limit %.0f) | "
					"TSAA-time c_forceExposure: forced on %llu of %llu TSAA frames, range %.4f..%.4f, stale %llu | b2 size rejects %llu | foreign-device events ignored %llu",
					cam::cuts, cam::deltas, cam::max_dpos, g_cfg.cam_cut_dist, cam::max_dang, g_cfg.cam_cut_deg, cam::max_dfov, g_cfg.cam_cut_fov_deg,
					g_fexp.forced, g_fexp.frames, g_fexp.lo > g_fexp.hi ? 0.0f : g_fexp.lo, g_fexp.lo > g_fexp.hi ? 0.0f : g_fexp.hi, g_fexp.invalid,
					cam::size_rejects, g_foreign_events.exchange(0));
				cam::updates = cam::snaps = cam::ok_count = cam::bad_count = cam::no_update = cam::size_mismatch = cam::published = 0;
				cam::cuts = cam::deltas = 0; cam::max_dpos = cam::max_dang = cam::max_dfov = 0.0f;
				g_fexp = {};
			}
			g_nested = 0;
			if (g_cfg.probe && g_st.tsaa_draws == 0)
				logf("   no TSAA draw in this window: a menu, or Settings > Video > Anti-aliasing is not TSAA (DLSS SR replaces TSAA and needs it on)");
			g_st = {};
			// one more detailed TSAA dump per window, for 8 windows (0.3.0 reset the counter every window, so it never ended)
			if (g_cfg.probe && g_probe_windows < 8) { ++g_probe_windows; g_probe_logs = 3; }
		}
	}

	void on_destroy_device(device *dev)
	{
		// 0.3.1: only the game's D3D11 device. renodx-dlss5's private D3D12 device (created through ReShade's hook) raises
		// destroy_device too; 0.3.0 then shut NGX down on the live game device and dropped the FG export mid-session.
		if (dev == nullptr || dev->get_api() != device_api::d3d11) return;
		{
			device *const gd = g_game_dev.load();
			if (gd != nullptr && dev != gd) return;
			if (gd == nullptr && g_gpu.dev != nullptr && reinterpret_cast<ID3D11Device *>(dev->get_native()) != g_gpu.dev) return;
		}
		SR_LOCK_ALWAYS();
		if (g_nrl_reset) g_nrl_reset();   // tf2-nr-linux holds views on our textures and a D3D12 session on this adapter
		if (eng::applied) { for (eng::cv *c : eng::all) eng::restore(*c); eng::applied = false; eng::applied_method = -1; logf("engine: device destroyed -> Adaptive Resolution values restored"); }
		if (g_ngx.feature) { NVSDK_NGX_D3D11_ReleaseFeature(g_ngx.feature); g_ngx.feature = nullptr; }
		if (g_ngx.params) { NVSDK_NGX_D3D11_DestroyParameters(g_ngx.params); g_ngx.params = nullptr; }
		if (g_ngx.inited) { NVSDK_NGX_D3D11_Shutdown1(g_gpu.dev); g_ngx.inited = false; }
		for (auto &kv : g_srv_cache) safe_release(kv.second);
		g_srv_cache.clear();
		mip::release_all();
		g_rejected.clear(); g_optimal.clear();
		g_ngx.mode = -1; g_ngx.force_exact = false;
		release_targets();   // also drops the FG export's references and bumps the generation
		safe_release(g_gpu.exposure); safe_release(g_gpu.cs); safe_release(g_gpu.params);
		cam::release();
		{ std::lock_guard lk(fgx::mx); fgx::s.cam_valid = false; }
		g_gpu.dev = nullptr;
		g_fg_native.store(nullptr); g_fg_mod = nullptr;   // re-resolved on the next present (add-ons may be reloaded)
		g_game_dev.store(nullptr);                         // learned again from the next device's first present
		logf("device destroyed - NGX shut down, DLSS resources and the FG export released");
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// FG export (bridge\tf2_fg_interop.h). Any thread; takes only fgx::mx; no D3D calls except AddRef; never throws.
// Returns 1 = VALID (depth / motion_vectors / exposure AddRef'd for the caller), 0 = not valid (no references),
// -1 = bad argument (null or struct_size < sizeof(tf2sr_fg_inputs_v1); nothing written), -2 = internal exception.
extern "C" __declspec(dllexport) int tf2sr_get_fg_inputs(tf2sr_fg_inputs_v1 *io)
{
	if (io == nullptr || io->struct_size < sizeof(tf2sr_fg_inputs_v1)) { fgx::calls_bad.fetch_add(1); return -1; }
	try
	{
		tf2sr_fg_inputs_v1 o = {};
		o.struct_size = sizeof(o);
		o.version = TF2SR_FG_INPUTS_VERSION;
		o.mv_scale_x = 1.0f; o.mv_scale_y = 1.0f;
		int ret = 0;
		{
			std::lock_guard lk(fgx::mx);
			const fgx::snap_t &s = fgx::s;
			const uint64_t now = fgx::sr_frame.load();
			o.exporter_frame = now;
			o.game_frame = s.frame;
			o.eval_serial = s.serial;
			o.generation = s.generation;
			o.sr_mode = s.mode;
			o.render_width = s.rw; o.render_height = s.rh;
			o.display_width = s.ow; o.display_height = s.oh;
			o.jitter_x_px = s.jx_px; o.jitter_y_px = s.jy_px;
			o.jitter_x_uv = s.jx_uv; o.jitter_y_uv = s.jy_uv;
			o.exposure_value = s.expo_forced;
			o.tsaa_render_width = s.trw; o.tsaa_render_height = s.trh;
			o.flags = s.flags & ~TF2SR_FG_VALID;
			if (s.cam_valid)
			{
				o.flags |= TF2SR_FG_CAMERA_VALID | (s.cam_far_inf ? TF2SR_FG_CAMERA_FAR_INF : 0u);
				o.camera_near = s.cam_near; o.camera_far = s.cam_far; o.camera_fov_y = s.cam_fovy; o.camera_aspect = s.cam_aspect;
			}
			o.camera_frame = s.camera_frame;
			const bool valid = s.valid && s.depth != nullptr && s.mv != nullptr && s.expo != nullptr && now >= s.frame && now - s.frame <= 1 &&
			                   s.generation == fgx::generation;
			if (valid)
			{
				o.flags |= TF2SR_FG_VALID;
				o.depth = s.depth; o.depth->AddRef();
				o.motion_vectors = s.mv; o.motion_vectors->AddRef();
				o.exposure = s.expo; o.exposure->AddRef();
				ret = 1;
			}
		}
		*io = o;
		fgx::calls.fetch_add(1);
		if (ret == 1) fgx::calls_valid.fetch_add(1);
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
		g_module = module;
		if (!reshade::register_addon(module)) return FALSE;
		wchar_t path[MAX_PATH] = {};
		GetModuleFileNameW(module, path, MAX_PATH);
		g_dir = std::filesystem::path(path).parent_path();
		g_log = _wfsopen((g_dir / "tf2-dlss-sr.log").c_str(), L"w", _SH_DENYNO);
		logf("tf2-dlss-sr 0.3.2 loaded (+ tf2-nr-linux route; 0.3.1 + renodx route needs renodx loaded + NGX search add-on folder then game folder; TSAA-clamp render size, no-TSAA guard, FG export tf2sr_get_fg_inputs v%u + camera-cut hint, tf2-fg native-present skip, game-device filter)", TF2SR_FG_INPUTS_VERSION);
		load_config();
		logf("backend: this build has no FSR 3 upscaler (the 0.4.x FSR 3 backend is held back) - tf2nr_upscaler is not read, DLSS is always used");
		refresh_route();   // logs the route (and again whenever it changes)
		reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
		reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
		reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
		reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_rts);
		reshade::register_event<reshade::addon_event::bind_viewports>(on_bind_viewports);
		reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
		reshade::register_event<reshade::addon_event::map_buffer_region>(on_map_buffer);
		reshade::register_event<reshade::addon_event::unmap_buffer_region>(on_unmap_buffer);
		reshade::register_event<reshade::addon_event::update_buffer_region>(on_update_buffer);
		reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
		reshade::register_event<reshade::addon_event::destroy_sampler>(on_destroy_sampler);
		reshade::register_event<reshade::addon_event::draw>(on_draw);
		reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::register_event<reshade::addon_event::dispatch>(on_dispatch);
		reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_indirect);
		reshade::register_event<reshade::addon_event::copy_resource>(on_copy_resource);
		reshade::register_event<reshade::addon_event::copy_texture_region>(on_copy_texture_region);
		reshade::register_event<reshade::addon_event::clear_render_target_view>(on_clear_rt);
		reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(on_clear_ds);
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
