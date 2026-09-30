// tf2_fg.cpp - tf2-fg 0.4: frame generation in Titanfall 2 (Northstar, D3D11, ReShade 6.8, API 20).
//
// 0.4.0 (M4): Presenter=2 "xa + DComp" (research\fg_hdr\pacing.md section 6; code in xa_presenter.hpp). On a hybrid laptop
//   (renders on the NVIDIA GPU, the window's display belongs to another adapter) both frames of a pair are presented from the
//   DISPLAY adapter: FSR writes G straight into a shared slot, the render thread copies R(k) into another and signals a
//   shared fence (no native Present, no GPU wait on the render thread); a pacer thread moves both through the RTX copy
//   engine and a cross-adapter heap to a composition flip swap chain on the display adapter, shown in a topmost
//   DirectComposition visual on the game window, G at once and R at t(G) + D/2. The game's own present keeps running
//   underneath (ReShade/SR/NR/bridge see one present per game frame, as without FG); detaching the visual shows it again.
//   Presenter=auto (default): 2 on the hybrid topology (pass-through - FG suspended - if presenter 2 fails: the in-queue
//   presenter shows ~12 % of G there), 1 (the in-queue presenter below) when the display is on the render adapter.
//   MaxBaseFps is capped at refresh / 2 with presenter 2 (a pair needs two refreshes). IsIconic -> suspend (detach).
//   Debug (F7): FsrDebugA / FsrDebugB bitmasks (1 = zero motion vectors, 2 = flat depth, 4 = optical flow off) alternate
//   every FsrDebugAB FSR frames (A, B, A, ...; each switch resets FSR); dumps log the phase.
//
// 0.3.1 (M3 review fixes):
//   - cuts: a CPU frame hitch at frame k also cuts frame k+1 (TF2's queued renderer puts the big simulation step on the
//     frame AFTER the long present interval: m3b showed the GPU guard firing at k+1 after 8 of 10 hitches); tf2-dlss-sr's
//     camera-cut hint (TF2SR_FG_CAMERA_CUT: the main view jumped in position / direction / fovY between two frames) resets
//     FSR, so death cam / kill replay / respawn cuts no longer depend on the luma guard alone;
//   - the library's own discontinuity rule (frameID gap, HUD-less presence change) is mirrored in `reset`, so those
//     reset dispatches (output = R(k)) are never presented and counted as G;
//   - FSR needs the SR export's conventions (jitter-free MVs, standard depth) and an RGBA8 back buffer, else no FSR;
//   - RaiseFeatureLevel only when FSR FG can run this session ([TF2FG] Enable=1 and Mode=3, both read at start);
//   - the FSR context is created only outside ACTIVE (WARMUP / SUSPENDED: no create hitch inside FG) and destroyed after
//     600 frames with FG OFF;
//   - the HUD-less export is also polled while SUSPENDED (the bridge's demand keep-alive: with DLSS 5 NR off its pre-HUD
//     cut no longer flips between pre-HUD and present on every ACTIVE/SUSPENDED change);
//   - log numbering: GPU cut-guard lines name the game frame k whose R(k-1)->R(k) pair was measured (0.3.0 printed k+1).
//
// 0.3.0 (M3, the product mode): Mode=3 = AMD FSR 3 frame interpolation (FidelityFX FI 1.1 + OF 1.1.1 on D3D11, the
// production library C:\dev\tf2-fsr-fg\lib\build\tf2fg_fsr.lib, lib\tf2fg_fsr.h). 0.2.0 is kept in fg\v0.2.0\.
//   - Inputs per game frame k, at reshade_present(k) (the back buffer holds R(k)):
//       tf2-dlss-sr export tf2sr_get_fg_inputs: render-size depth (R32F, standard Z), jitter-free MVs (RG16F, render px,
//       cur + mv = prev), jitter, render size, camera near/far/fovY (CBufCommonPerCamera b2 decode; far 1e7 = infinite);
//       tf2-nr-bridge export tf2nr_get_hudless: the back buffer copied right before the first HUD draw (HUD-less R(k)).
//     prepare(depth, mv, camera, frameID = k) then dispatch(currentBackBuffer = real[k] (= R(k) with HUD), hudless(k),
//     output = gen): gen = the frame half-way between R(k-1) and R(k), motion-compensated, with R(k)'s HUD composited by
//     FSR itself (HUD-less diff). sRGB: every colour texture is an RGBA8 TYPELESS copy registered as UNORM, FSR decodes the
//     sRGB bytes itself (Transfer::Srgb); gen holds sRGB-encoded bytes and is copied raw into the _SRGB back buffer.
//   - The FSR context is created on the NATIVE device (device::get_native(), never the proxy) and only when the device is
//     FL >= 11_1 (FI's inpainting pyramid binds 13 UAVs). RaiseFeatureLevel=1 requests 11_1 at device creation. With a
//     11_0 device and FG enabled, the log says "restart required" and FG stays OFF.
//   - FG needs a VALID SR export this frame (DLSS evaluated: SR on, or DLAA with NR on); otherwise the frame passes
//     through (the game's present shows R(k)) and 30 frames without it suspend FG ("no DLSS SR inputs").
//   - Resets (FSR history restarted; the reset frame's output is R(k) itself, so NO G is presented for it): SR RESET flag
//     (camera cut / TSAA gap / new DLSS feature), an export generation change, a CPU cut (hitch, main-pass size change),
//     FG (re)activation, missing SR inputs, library errors; the library also resets on a frameID gap and on a HUD-less
//     presence change. The GPU cut guard stays: a diff pass before FSR, a select pass after it (gen = cur on a cut).
//   - Library failures: the context is destroyed and re-created after 600 frames, at most 3 times per session; Error-level
//     library log lines are counted ("library errors" in the window log). FSR GPU time from the library's timestamps.
//   - Marker is a bitmask: 1 = magenta 48x48 square on G (M1a), 2 = the fgmark.h barcode (16 cells at (64,0): type R/G +
//     12-bit present sequence 2*frame + isR) on EVERY presented frame, FG on or off, for fg\tools\dupmon display-delivery
//     measurements. DumpTriplet also reads back the HUD-less image (Mode 3) and DumpMinChanged=P only dumps frames whose
//     latest cut-guard read-back shows >= P % changed samples (moving camera).
//   - Presenter unchanged from 0.2 (R(k) by a native Present at frame k+1's pre_dlss anchor, gates, fps hysteresis).
//     The pacing track's cross-adapter 780M + DComp presenter (research\fg_hdr\pacing.md sec. 6) is the next milestone.
//
// 0.2.0 (review fixes of 0.1.0; the 0.1.0 source is kept in fg\v0.1.0\ for diffing):
//   - gate: RequireTSAA=1 by default (the pre/post anchors exist only in frames with a TSAA pass, i.e. DLSS SR or in-game
//     TSAA on); without one FG suspends ("no TSAA anchor") instead of presenting R at the deadline, just before G;
//   - camera cuts: no G on a CPU-detected cut (frame hitch, main-pass size change) and, in Mode 2, a GPU cut guard
//     (a diff pass; gen = cur when more than CutPercent % of the sampled pixels changed a lot);
//   - foreign present hooks: callbacks raised on our thread while our native Present runs are counted; any such event
//     suspends FG for the session (AllowPresentHooks=1 overrides), exported tf2fg_in_native_present() for other add-ons,
//     module rescan every 600 frames, the native Present entry point (vtable target / inline jmp) is logged;
//   - MinBaseFps hysteresis: resume only when the PREDICTED FG-on fps (FG-off fps x measured on/off ratio) clears
//     MinBaseFps + 10 %;
//   - convar 0 is really off: with FG OFF and Profile=0 the per-draw callbacks return at once, no timestamps;
//   - menu convars are looked up only 20 s after the first gameplay frame (no FindVar near Host_Init) and polling stops
//     after 60 s without the convar;
//   - destroy events always clean up; every callback body is wrapped in try/catch; the log is closed under its lock;
//   - V-Sync: mat_vsync_mode is read from videoconfig.txt; FG suspends while V-Sync is on (the R present would not
//     match the game's sync interval);
//   - resource-creation failures are latched (600 frames), RaiseFeatureLevel only for the first D3D11 device and only
//     on ReShade 6.8.x, the effect runtime is matched to our swap chain, D3D11 predication is lifted around our copies,
//     Mode=2 is the default, DebugR=2 no longer counts a Flush as a present, a back buffer left bound across frames
//     triggers the deadline on the first draw, pipeline tags are cleared with the device.
//
// Design: research\fg_hdr\dx11_presenter.md sections 1-7, with the corrections in research\fg_hdr\verifier_findings.md.
// "GPU-timeline present injection". At the end of game frame k (reshade_present) the back buffer holds the real frame
// R(k). We save it, build a generated frame G in our own texture, overwrite the back buffer with G and let ReShade's
// proxied Present show G. R(k) is shown by a NATIVE IDXGISwapChain::Present (swapchain->get_native() = ReShade's _orig,
// never the proxy) that we issue from inside frame k+1's command stream at an anchor event. On the in-order D3D11
// queue, R(k) then reaches the screen part-way through frame k+1's GPU work. The render thread never waits.
//
//   M1a  Mode=1  G = copy of R(k-1), plus a magenta 48x48 ClearView marker at the top left (Marker=1). Copies and
//                ClearView only, no pipeline state.
//   M1b  Mode=2  G = lerp(R(k-1), R(k), BlendT) in linear light: a compute shader run inside SwapDeviceContextState
//                (a private context state at the device's own feature level), or manual CS save/restore (StateMode=1).
//   M3   Mode=3  G = FSR 3 frame interpolation (default since 0.3.0; see above).
//
// Anchors (InjectAt):
//   1 pre_dlss  = the bind_pipeline event that binds TF2's TSAA pixel shader. tf2-dlss-sr loads before us (add-ons load
//                 alphabetically) and runs DLSS+NR in its callback of the TSAA DRAW event, which comes later. A present
//                 here therefore lands before the DLSS/NR chunk; a present at the TSAA draw event would land after it.
//   2 post_dlss = the first bind/draw/dispatch/copy/clear event after the TSAA draw. DLSS, NR, the TSAA draw and SR's
//                 overwrite copy are all queued before it.
//   3 deadline  = the deadline only.
//   0 auto      = experimental (M2a preview): once per log window, pick the anchor whose measured GPU offset from G is
//                 closest to D/2 + PaceBiasMs.
//   The deadline always applies: the first event of frame k+1 that would write the back buffer. That is a render-target
//   bind of the back buffer (the event fires after OMSetRenderTargets and before any draw), or a clear / copy / resolve /
//   update whose destination is the back buffer (these events fire before the native call). If R(k) is still pending
//   there, it is presented there.
//   Late R: if reshade_present(k+1) arrives with R(k) still pending, R(k) is presented first (counted), then G(k+1/2).
//   If frame k+1 is not an FG frame, a still-pending R(k) is dropped instead (counted).
//
// Gameplay gate (per frame): the main scene pass (RGBA16 + RG16 MRT) and the engine_post draw were seen this frame,
// and (RequireTSAA=1, the default) the TSAA draw: the pre_dlss/post_dlss anchors only exist in frames with a TSAA pass
// (DLSS SR on, or the in-game TSAA). RequireTSAA=0 lets FG run without it; R is then presented at the deadline, right
// before G, and is mostly never seen. FG never runs in menus or loading.
// Effective FG = [TF2FG] Enable=1 AND the menu convar tf2nr_fsr_fg = 1 (TF2NR.GraphicsToggles >= 0.4.0). The convar is
// looked up 20 s after the first gameplay frame (well past Host_Init; tf2nr_nr_tuning.hpp), then read every frame.
//
// Config [TF2FG] in ReShade.ini (read at start):
//   Enable=0  Mode=3  InjectAt=1 (or auto|pre_dlss|post_dlss|deadline)  PaceBiasMs=0  MinBaseFps=40  MaxBaseFps=83
//   Marker=0 (bitmask 1 magenta G square, 2 barcode)  LogWindow=600  RaiseFeatureLevel=1  SyncInterval=0  RequireTSAA=1
//   BlendT=0.5  StateMode=0
//   FsrHudless=1 (pass the bridge's HUD-less copy; 0 = none: FSR interpolates the HUD too)  FsrTiming=1 (GPU timestamps
//   around OF+FI)  FsrDebugView=0  FsrMinStackKB=256  FsrCamHoldFrames=30 (reuse the last decoded camera this many
//   frames when a frame has none)  DumpMinChanged=0
//   WarmupFrames=60  ResumeFrames=30  Profile=0 (1 = GPU timestamps + window logs even while FG is off)
//   CutPercent=50 (Mode 2 GPU cut guard: gen = cur when more than this % of sampled pixels changed; 0 = off)
//   CutDelta=0.12 (perceptual luma change counted as "changed")  CutHitchMs=40 (CPU cut: frame interval above this AND
//   above 2.5x the average)  AllowPresentHooks=0 (1 = keep FG on even when a present hook renders inside our Present)
//   IgnoreVsync=0 (1 = do not suspend while videoconfig says V-Sync is on)
//   DumpTriplet=0 (N > 0: read back real[k-1], gen, real[k] to PNGs next to the log at the N-th generated frame)
//   DumpEvery=0 (repeat every M generated frames)  DumpMax=1  DumpKey=0 (VK code: dump on a key press)
//   RaiseFeatureLevel=1: in the create_device event, ask for D3D11 FL 11_1 for the game's device, after checking
//   support with D3D11CreateDevice(ppDevice = NULL) on the default adapter. Unsupported -> left unchanged.
//
// Crash safety. Northstar's first-in-chain VEH kills the game on any access violation, integer divide by zero,
// breakpoint or invalid handle, even inside __try. So:
//   - every pointer is validated before use, and native objects are used only while their generation matches;
//   - there is no integer division by a runtime value;
//   - no lock is held across the native Present;
//   - a thread_local guard makes every callback return while our own native Present runs (third-party Present hooks can
//     render through ReShade's proxies); such events are counted and switch FG off for the session;
//   - every callback body is wrapped in try/catch; a C++ exception switches FG off for the session;
//   - destroy events are never skipped by the re-entrancy guard (stale swap-chain pointers would be fatal).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <dcomp.h>
#include <atomic>
#include <new>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <share.h>
#include <mutex>
#include <atomic>
#include <chrono>
#include <vector>
#include <string>
#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <shlobj.h>
#include <psapi.h>
#include <reshade.hpp>
#include "blend_cs.h"   // const BYTE g_blend_cs[] (fxc /E blend_main /Vn g_blend_cs)
#include "diff_cs.h"    // const BYTE g_diff_cs[]  (fxc /E diff_main /Vn g_diff_cs)
#include "select_cs.h"  // const BYTE g_select_cs[] (fxc /E select_main /Vn g_select_cs)
#include "../bridge/tf2nr_menu_toggles.hpp"
#include "../bridge/tf2_fg_interop.h"
#include "tf2fg_fsr.h"  // C:\dev\tf2-fsr-fg\lib (production lib\build\tf2fg_fsr.lib)

extern "C" __declspec(dllexport) const char *NAME = "TF2 FG";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Titanfall 2: frame generation (AMD FSR 3 frame interpolation; GPU-timeline present injection).";

using namespace reshade::api;

namespace
{
	constexpr const char *kVersion = "0.4.1";
	constexpr const char *kSection = "TF2FG";

	HMODULE g_module = nullptr;
	std::filesystem::path g_dir;
	FILE *g_log = nullptr;
	std::mutex g_log_mx;

	void logf(const char *fmt, ...)
	{
		std::lock_guard lock(g_log_mx);
		if (!g_log) return;   // checked under the lock: DLL_PROCESS_DETACH closes it under the same lock
		SYSTEMTIME t; GetLocalTime(&t);
		LARGE_INTEGER q = {}, f = {}; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
		// q = QPC in ms: the same clock as dupmon's qpc_present (split display captures by FG state)
		fprintf(g_log, "%02u:%02u:%02u.%03u q%.1f ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
			f.QuadPart > 0 ? static_cast<double>(q.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart) : 0.0);
		va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
		fputc('\n', g_log); fflush(g_log);
	}

	// Re-entrancy. t_in_native: our own native Present runs on this thread (an overlay hooked on the real swap chain may
	// render through ReShade's proxy device/context and raise events into every add-on, including this one; its bind of
	// the back buffer would otherwise hit our own deadline branch). Such events are counted in t_foreign: tf2-dlss-sr and
	// tf2-nr-bridge get the same events and have no guard, so a non-zero count switches FG off (AllowPresentHooks=0).
	// t_depth: an event raised while one of our callbacks is on the stack. Both make every callback return immediately.
	thread_local bool t_in_native = false;
	thread_local uint32_t t_foreign = 0;
	thread_local int t_depth = 0;
	struct depth_guard { depth_guard() { ++t_depth; } ~depth_guard() { --t_depth; } };
#define FG_ENTER(...) if (t_in_native) { ++t_foreign; return __VA_ARGS__; } if (t_depth != 0) { return __VA_ARGS__; } depth_guard fg_depth_guard_
	void handle_exception(const char *where);
#define FG_TRY try {
#define FG_CATCH(...) } catch (...) { handle_exception(__func__); return __VA_ARGS__; }

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
	template <class T> void safe_release(T *&p) { if (p) { p->Release(); p = nullptr; } }

	double qpc_ms()
	{
		static LARGE_INTEGER f = {};
		if (f.QuadPart == 0) QueryPerformanceFrequency(&f);
		LARGE_INTEGER c; QueryPerformanceCounter(&c);
		return f.QuadPart > 0 ? static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart) : 0.0;
	}

	// ------------------------------------------------------------------ config
	enum anchor_kind : int { A_NONE = 0, A_PRE = 1, A_POST = 2, A_DEAD = 3, A_LATE = 4 };
	const char *anchor_name(int a)
	{
		switch (a) { case A_PRE: return "pre_dlss"; case A_POST: return "post_dlss"; case A_DEAD: return "deadline"; case A_LATE: return "late"; default: return "auto"; }
	}

	struct cfg_t
	{
		int enable = 0, mode = 3, inject_at = A_PRE, marker = 0, log_window = 600, raise_fl = 1, sync_interval = 0;
		int require_tsaa = 1, state_mode = 0, warmup = 60, resume = 30, profile = 0;
		int dump_triplet = 0, dump_every = 0, dump_max = 1, dump_key = 0;
		int max_frame_latency = 0, debug_skip_r = 0, no_wait = 0;
		int allow_present_hooks = 0, ignore_vsync = 0;
		int fsr_hudless = 1, fsr_timing = 1, fsr_debug_view = 0, fsr_min_stack_kb = 256, fsr_cam_hold = 30;
		float pace_bias_ms = 0.0f, min_fps = 40.0f, max_fps = 83.0f, blend_t = 0.5f;
		float cut_percent = 50.0f, cut_delta = 0.12f, cut_hitch_ms = 40.0f, dump_min_changed = 0.0f;
		// 0.4.0
		int presenter = -1;                        // -1 auto, 0 pass-through, 1 in-queue (M1), 2 xa + DComp
		int xa_spin_us = 500, xa_device = 0, present_log = 0, xa_log_window = 0;
		int fsr_dbg_a = 0, fsr_dbg_b = 0, fsr_dbg_ab = 0;   // bitmask 1 zero MVs, 2 flat depth, 4 optical flow off; AB = frames per phase
		// 0.4.1 (debug / measurement)
		int xa_test_build_teardown = 0, xa_test_stall_ms = 0, xa_test_stall_at = 1500;
		int cost_ab = 0;                           // XaCostAB = seconds per phase: OFF -> FSR only (no presenter) -> presenter 2, cycling
		float xa_off_clamp_ms = 2.0f;
		int xa_g_hold = 1, xa_g_hold_ab = 0;       // XaGHold (0 = half a refresh after R as 0.4.0, 1 = a full refresh), XaGHoldAB (debug)
	} g_cfg;

	int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
	float clampf(float v, float lo, float hi) { return !(v >= lo) ? lo : (v > hi ? hi : v); }

	void load_config()
	{
		reshade::get_config_value(nullptr, kSection, "Enable", g_cfg.enable);
		reshade::get_config_value(nullptr, kSection, "Mode", g_cfg.mode);
		{
			char buf[32] = {}; size_t len = sizeof(buf);
			if (reshade::get_config_value(nullptr, kSection, "InjectAt", buf, &len))
			{
				if (buf[0] >= '0' && buf[0] <= '9') g_cfg.inject_at = atoi(buf);
				else if (_stricmp(buf, "auto") == 0) g_cfg.inject_at = 0;
				else if (_stricmp(buf, "pre_dlss") == 0) g_cfg.inject_at = A_PRE;
				else if (_stricmp(buf, "post_dlss") == 0) g_cfg.inject_at = A_POST;
				else if (_stricmp(buf, "deadline") == 0) g_cfg.inject_at = A_DEAD;
			}
		}
		reshade::get_config_value(nullptr, kSection, "PaceBiasMs", g_cfg.pace_bias_ms);
		reshade::get_config_value(nullptr, kSection, "MinBaseFps", g_cfg.min_fps);
		reshade::get_config_value(nullptr, kSection, "MaxBaseFps", g_cfg.max_fps);
		reshade::get_config_value(nullptr, kSection, "Marker", g_cfg.marker);
		reshade::get_config_value(nullptr, kSection, "LogWindow", g_cfg.log_window);
		reshade::get_config_value(nullptr, kSection, "RaiseFeatureLevel", g_cfg.raise_fl);
		reshade::get_config_value(nullptr, kSection, "SyncInterval", g_cfg.sync_interval);
		reshade::get_config_value(nullptr, kSection, "RequireTSAA", g_cfg.require_tsaa);
		reshade::get_config_value(nullptr, kSection, "BlendT", g_cfg.blend_t);
		reshade::get_config_value(nullptr, kSection, "StateMode", g_cfg.state_mode);
		reshade::get_config_value(nullptr, kSection, "WarmupFrames", g_cfg.warmup);
		reshade::get_config_value(nullptr, kSection, "ResumeFrames", g_cfg.resume);
		reshade::get_config_value(nullptr, kSection, "Profile", g_cfg.profile);
		reshade::get_config_value(nullptr, kSection, "DumpTriplet", g_cfg.dump_triplet);
		reshade::get_config_value(nullptr, kSection, "DumpEvery", g_cfg.dump_every);
		reshade::get_config_value(nullptr, kSection, "DumpMax", g_cfg.dump_max);
		reshade::get_config_value(nullptr, kSection, "DumpKey", g_cfg.dump_key);
		reshade::get_config_value(nullptr, kSection, "MaxFrameLatency", g_cfg.max_frame_latency);
		reshade::get_config_value(nullptr, kSection, "DebugR", g_cfg.debug_skip_r);
		reshade::get_config_value(nullptr, kSection, "NoWait", g_cfg.no_wait);
		reshade::get_config_value(nullptr, kSection, "AllowPresentHooks", g_cfg.allow_present_hooks);
		reshade::get_config_value(nullptr, kSection, "IgnoreVsync", g_cfg.ignore_vsync);
		reshade::get_config_value(nullptr, kSection, "CutPercent", g_cfg.cut_percent);
		reshade::get_config_value(nullptr, kSection, "CutDelta", g_cfg.cut_delta);
		reshade::get_config_value(nullptr, kSection, "CutHitchMs", g_cfg.cut_hitch_ms);
		reshade::get_config_value(nullptr, kSection, "FsrHudless", g_cfg.fsr_hudless);
		reshade::get_config_value(nullptr, kSection, "FsrTiming", g_cfg.fsr_timing);
		reshade::get_config_value(nullptr, kSection, "FsrDebugView", g_cfg.fsr_debug_view);
		reshade::get_config_value(nullptr, kSection, "FsrMinStackKB", g_cfg.fsr_min_stack_kb);
		reshade::get_config_value(nullptr, kSection, "FsrCamHoldFrames", g_cfg.fsr_cam_hold);
		reshade::get_config_value(nullptr, kSection, "DumpMinChanged", g_cfg.dump_min_changed);
		{
			char buf[32] = {}; size_t len = sizeof(buf);
			if (reshade::get_config_value(nullptr, kSection, "Presenter", buf, &len))
			{
				if (_stricmp(buf, "auto") == 0) g_cfg.presenter = -1;
				else if (buf[0] >= '0' && buf[0] <= '2' && buf[1] == 0) g_cfg.presenter = buf[0] - '0';
				else if (_stricmp(buf, "xa") == 0) g_cfg.presenter = 2;
				else if (_stricmp(buf, "inqueue") == 0) g_cfg.presenter = 1;
			}
			len = sizeof(buf); buf[0] = 0;
			if (reshade::get_config_value(nullptr, kSection, "XaDevice", buf, &len)) g_cfg.xa_device = _stricmp(buf, "create") == 0 ? 1 : 0;
		}
		reshade::get_config_value(nullptr, kSection, "XaSpinUs", g_cfg.xa_spin_us);
		g_cfg.present_log = -1;   // default: on with the Marker=2 barcode (a test mode: the csv joins with dupmon's barcode reads)
		reshade::get_config_value(nullptr, kSection, "PresentLog", g_cfg.present_log);
		if (g_cfg.present_log < 0) g_cfg.present_log = (g_cfg.marker & 2) ? 1 : 0;
		reshade::get_config_value(nullptr, kSection, "XaLogWindow", g_cfg.xa_log_window);
		reshade::get_config_value(nullptr, kSection, "FsrDebugA", g_cfg.fsr_dbg_a);
		reshade::get_config_value(nullptr, kSection, "FsrDebugB", g_cfg.fsr_dbg_b);
		reshade::get_config_value(nullptr, kSection, "FsrDebugAB", g_cfg.fsr_dbg_ab);
		reshade::get_config_value(nullptr, kSection, "XaTestTeardownInBuild", g_cfg.xa_test_build_teardown);
		reshade::get_config_value(nullptr, kSection, "XaTestStallMs", g_cfg.xa_test_stall_ms);
		reshade::get_config_value(nullptr, kSection, "XaTestStallAtJob", g_cfg.xa_test_stall_at);
		reshade::get_config_value(nullptr, kSection, "XaCostAB", g_cfg.cost_ab);
		reshade::get_config_value(nullptr, kSection, "XaOffsetClampMs", g_cfg.xa_off_clamp_ms);
		reshade::get_config_value(nullptr, kSection, "XaGHold", g_cfg.xa_g_hold);
		reshade::get_config_value(nullptr, kSection, "XaGHoldAB", g_cfg.xa_g_hold_ab);
		g_cfg.xa_test_build_teardown = clampi(g_cfg.xa_test_build_teardown, 0, 1000);
		g_cfg.xa_test_stall_ms = clampi(g_cfg.xa_test_stall_ms, 0, 30000);
		g_cfg.xa_test_stall_at = clampi(g_cfg.xa_test_stall_at, 1, 100000000);
		g_cfg.cost_ab = clampi(g_cfg.cost_ab, 0, 3600);
		g_cfg.xa_off_clamp_ms = clampf(g_cfg.xa_off_clamp_ms, 0.0f, 4.0f);
		g_cfg.xa_g_hold = clampi(g_cfg.xa_g_hold, 0, 1);
		g_cfg.xa_g_hold_ab = clampi(g_cfg.xa_g_hold_ab, 0, 3600);
		g_cfg.xa_spin_us = clampi(g_cfg.xa_spin_us, 0, 5000);
		g_cfg.fsr_dbg_a = clampi(g_cfg.fsr_dbg_a, 0, 7); g_cfg.fsr_dbg_b = clampi(g_cfg.fsr_dbg_b, 0, 7);
		g_cfg.fsr_dbg_ab = clampi(g_cfg.fsr_dbg_ab, 0, 100000);
		g_cfg.fsr_min_stack_kb = clampi(g_cfg.fsr_min_stack_kb, 64, 4096);
		g_cfg.fsr_cam_hold = clampi(g_cfg.fsr_cam_hold, 0, 600);
		g_cfg.dump_min_changed = clampf(g_cfg.dump_min_changed, 0.0f, 100.0f);
		g_cfg.marker = clampi(g_cfg.marker, 0, 3);
		g_cfg.cut_percent = clampf(g_cfg.cut_percent, 0.0f, 100.0f);
		g_cfg.cut_delta = clampf(g_cfg.cut_delta, 0.01f, 1.0f);
		g_cfg.cut_hitch_ms = clampf(g_cfg.cut_hitch_ms, 0.0f, 10000.0f);
		g_cfg.max_frame_latency = clampi(g_cfg.max_frame_latency, 0, 16);
		g_cfg.mode = clampi(g_cfg.mode, 1, 3);
		g_cfg.inject_at = clampi(g_cfg.inject_at, 0, 3);
		g_cfg.log_window = clampi(g_cfg.log_window, 60, 100000);
		g_cfg.xa_log_window = g_cfg.xa_log_window > 0 ? clampi(g_cfg.xa_log_window, 60, 100000) : g_cfg.log_window;
		g_cfg.sync_interval = clampi(g_cfg.sync_interval, 0, 4);
		g_cfg.warmup = clampi(g_cfg.warmup, 1, 10000);
		g_cfg.resume = clampi(g_cfg.resume, 1, 10000);
		g_cfg.dump_max = clampi(g_cfg.dump_max, 1, 20);
		g_cfg.dump_every = clampi(g_cfg.dump_every, 0, 1000000);
		g_cfg.pace_bias_ms = clampf(g_cfg.pace_bias_ms, -50.0f, 50.0f);
		g_cfg.min_fps = clampf(g_cfg.min_fps, 0.0f, 1000.0f);
		g_cfg.max_fps = clampf(g_cfg.max_fps, 1.0f, 10000.0f);
		g_cfg.blend_t = clampf(g_cfg.blend_t, 0.0f, 1.0f);
		logf("config: Enable=%d Mode=%d InjectAt=%d(%s) PaceBiasMs=%.2f MinBaseFps=%.1f MaxBaseFps=%.1f Marker=%d LogWindow=%d RaiseFeatureLevel=%d SyncInterval=%d",
			g_cfg.enable, g_cfg.mode, g_cfg.inject_at, anchor_name(g_cfg.inject_at), g_cfg.pace_bias_ms, g_cfg.min_fps, g_cfg.max_fps, g_cfg.marker,
			g_cfg.log_window, g_cfg.raise_fl, g_cfg.sync_interval);
		logf("config: RequireTSAA=%d BlendT=%.2f StateMode=%d WarmupFrames=%d ResumeFrames=%d Profile=%d DumpTriplet=%d DumpEvery=%d DumpMax=%d DumpKey=%d",
			g_cfg.require_tsaa, g_cfg.blend_t, g_cfg.state_mode, g_cfg.warmup, g_cfg.resume, g_cfg.profile, g_cfg.dump_triplet, g_cfg.dump_every,
			g_cfg.dump_max, g_cfg.dump_key);
		logf("config: CutPercent=%.1f CutDelta=%.3f CutHitchMs=%.1f AllowPresentHooks=%d IgnoreVsync=%d", g_cfg.cut_percent, g_cfg.cut_delta,
			g_cfg.cut_hitch_ms, g_cfg.allow_present_hooks, g_cfg.ignore_vsync);
		logf("config: MaxFrameLatency=%d (0 = leave the game's) NoWait=%d DebugR=%d%s", g_cfg.max_frame_latency, g_cfg.no_wait, g_cfg.debug_skip_r,
			g_cfg.debug_skip_r == 1 ? " (DEBUG: R is copied but never presented - measurement only)" :
			(g_cfg.debug_skip_r == 2 ? " (DEBUG: R is copied, then ID3D11DeviceContext::Flush instead of Present - measurement only, not counted as a present)" : ""));
		logf("config: FsrHudless=%d FsrTiming=%d FsrDebugView=%d FsrMinStackKB=%d FsrCamHoldFrames=%d DumpMinChanged=%.1f | Mode %d = %s", g_cfg.fsr_hudless,
			g_cfg.fsr_timing, g_cfg.fsr_debug_view, g_cfg.fsr_min_stack_kb, g_cfg.fsr_cam_hold, g_cfg.dump_min_changed, g_cfg.mode,
			g_cfg.mode == 3 ? "FSR 3 frame interpolation" : (g_cfg.mode == 2 ? "linear blend (M1b)" : "duplicate (M1a)"));
		logf("config: Presenter=%s XaSpinUs=%d XaDevice=%s PresentLog=%d XaLogWindow=%d | FsrDebugA=%d FsrDebugB=%d FsrDebugAB=%d%s",
			g_cfg.presenter < 0 ? "auto" : (g_cfg.presenter == 2 ? "2 (xa + DComp)" : (g_cfg.presenter == 1 ? "1 (in-queue)" : "0 (pass-through)")), g_cfg.xa_spin_us,
			g_cfg.xa_device ? "create (D3D12CreateDevice singleton)" : "factory (ID3D12DeviceFactory)", g_cfg.present_log, g_cfg.xa_log_window, g_cfg.fsr_dbg_a, g_cfg.fsr_dbg_b, g_cfg.fsr_dbg_ab,
			(g_cfg.fsr_dbg_a || (g_cfg.fsr_dbg_ab && g_cfg.fsr_dbg_b)) ? " (DEBUG FSR INPUTS: 1 zero motion vectors, 2 flat depth, 4 optical flow off)" : "");
		logf("config: XaGHold=%d XaGHoldAB=%d XaOffsetClampMs=%.2f XaCostAB=%d XaTestTeardownInBuild=%d XaTestStallMs=%d XaTestStallAtJob=%d",
			g_cfg.xa_g_hold, g_cfg.xa_g_hold_ab, g_cfg.xa_off_clamp_ms, g_cfg.cost_ab, g_cfg.xa_test_build_teardown, g_cfg.xa_test_stall_ms, g_cfg.xa_test_stall_at);
	}

	// ------------------------------------------------------------------ pipeline tags (any thread)
	constexpr uint32_t TAG_TSAA = 1u, TAG_EPOST = 2u;
	std::mutex g_tag_mx;
	std::unordered_map<uint64_t, uint32_t> g_tags;

	uint32_t tag_of(uint64_t p)
	{
		if (p == 0) return 0;
		std::lock_guard lock(g_tag_mx);
		auto it = g_tags.find(p);
		return it != g_tags.end() ? it->second : 0;
	}

	// ------------------------------------------------------------------ state (render thread / immediate context)
	enum class st_t { off, warmup, active, suspended };
	const char *st_name(st_t s)
	{
		switch (s) { case st_t::off: return "OFF"; case st_t::warmup: return "WARMUP"; case st_t::active: return "ACTIVE"; default: return "SUSPENDED"; }
	}

	enum ts_kind : int { TS_E = 0, TS_G0, TS_G, TS_FP, TS_PRE, TS_POST, TS_DEAD, TS_R, TS_RL, TS_COUNT };
	constexpr int kRing = 16;

	struct rec_q
	{
		ID3D11Query *dj = nullptr;
		ID3D11Query *ts[TS_COUNT] = {};
		uint64_t id = 0;
		uint32_t issued = 0;
		bool open = false, pending_read = false;
		int r_kind = A_NONE;      // anchor that presented R (the previous frame's) inside this bracket's command stream
		bool late = false;        // a late R was presented at the start of this bracket
		bool g_active = false;    // G was built at the start of this bracket (reshade_present of the previous frame)
	};
	struct rec_res
	{
		uint64_t id = ~0ull;
		bool ok = false, disjoint = true;
		uint64_t freq = 0;
		uint64_t ts[TS_COUNT] = {};
		uint32_t mask = 0;
		int r_kind = A_NONE;
		bool late = false;
		bool g_active = false;
	};

	struct frame_flags
	{
		bool main = false, epost = false, tsaa = false, after_tsaa = false, pre = false, post = false, dead = false;
		uint32_t main_w = 0, main_h = 0;   // main-pass MRT size (a change between frames = cut)
	};

	struct window_t
	{
		uint64_t frames = 0, fg_frames = 0, g_built = 0, r_pre = 0, r_post = 0, r_dead = 0, r_late = 0, r_dropped = 0;
		uint64_t occluded = 0, dxgi_err = 0, suspends = 0, disjoint = 0, lost = 0, samples = 0, gameplay = 0, tsaa = 0;
		uint64_t native_presents = 0, foreign = 0, cut_cpu = 0, cut_gpu = 0, cut_samples = 0, flushes = 0;
		float cut_frac_max = 0.0f; double cut_frac_sum = 0.0;
		double start_ms = 0.0, present_cpu_ms = 0.0, present_cpu_max = 0.0;
		std::vector<float> g2r, r2g, d, pre, post, dead, chunk, fp, gbuild, send;
		std::vector<float> c_proxy, c_dlss, c_to_pre, c_frame;   // CPU ms: proxied Present, pre->post anchor, frame start->pre anchor, present->present
		std::vector<float> c_rpres;                              // CPU ms of each native R Present call (incl. failed DO_NOT_WAIT tries)
		uint64_t wsd = 0, retries_ok = 0;                        // NoWait: WAS_STILL_DRAWING results, R presented on a retry
		// Mode 3 (FSR)
		uint64_t fsr_disp = 0, fsr_reset = 0, fsr_g = 0, fsr_nosr = 0, fsr_nohud = 0, fsr_camfb = 0, fsr_nocam = 0, fsr_err = 0, fsr_lib_err = 0;
		uint64_t fsr_nopair = 0, sr_valid = 0, sr_polls = 0, hud_valid = 0, hud_polls = 0;
		uint64_t fsr_camcut = 0, fsr_disc = 0, fsr_noctx = 0, sr_conv = 0;   // 0.3.1: SR camera-cut resets, library-rule resets, no context, SR convention refusals
		std::vector<float> fsr_gpu, fsr_cpu;                     // OF+FI GPU ms (library timestamps), CPU ms of prepare+dispatch
		// 0.4.0: presenter 2 (render thread) + our reshade_present CPU
		uint64_t xa_jobs = 0, xa_jobs_g = 0, xa_busy = 0, xa_gdrop = 0, xa_detreq = 0, xa_min = 0;
		uint64_t fore = 0;                                       // 0.4.1: frames with the game window in the foreground
		std::vector<float> c_rp;                                 // CPU ms of tf2-fg's reshade_present callback (all FG work on the render thread)
		void reset(double now)
		{
			const auto keep = [](std::vector<float> &v) { v.clear(); };
			frames = fg_frames = g_built = r_pre = r_post = r_dead = r_late = r_dropped = 0;
			occluded = dxgi_err = suspends = disjoint = lost = samples = gameplay = tsaa = native_presents = 0;
			foreign = cut_cpu = cut_gpu = cut_samples = flushes = 0; cut_frac_max = 0.0f; cut_frac_sum = 0.0;
			start_ms = now; present_cpu_ms = 0.0; present_cpu_max = 0.0;
			keep(g2r); keep(r2g); keep(d); keep(pre); keep(post); keep(dead); keep(chunk); keep(fp); keep(gbuild); keep(send);
			keep(c_proxy); keep(c_dlss); keep(c_to_pre); keep(c_frame); keep(c_rpres); wsd = retries_ok = 0;
			fsr_disp = fsr_reset = fsr_g = fsr_nosr = fsr_nohud = fsr_camfb = fsr_nocam = fsr_err = fsr_lib_err = 0;
			fsr_nopair = sr_valid = sr_polls = hud_valid = hud_polls = 0;
			fsr_camcut = fsr_disc = fsr_noctx = sr_conv = 0;
			keep(fsr_gpu); keep(fsr_cpu);
			xa_jobs = xa_jobs_g = xa_busy = xa_gdrop = xa_detreq = xa_min = fore = 0; keep(c_rp);
		}
	};

	struct fg_t
	{
		// device (the swap chain's device)
		device *rdev = nullptr;
		ID3D11Device *dev = nullptr;
		ID3D11Device1 *dev1 = nullptr;
		ID3D11DeviceContext *imm = nullptr;
		ID3D11DeviceContext1 *imm1 = nullptr;
		D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
		UINT dev_flags = 0;
		bool first_d3d11_device_logged = false;

		// swap chain
		swapchain *rsc = nullptr;
		IDXGISwapChain *native_sc = nullptr;   // ReShade's _orig; NOT AddRef'd (ReShade checks its refcount at release)
		uint64_t sc_gen = 1;
		uint64_t bb = 0;                        // ID3D11Texture2D* of buffer 0 (stable identity on D3D11)
		effect_runtime *rt = nullptr;
		DWORD render_tid = 0;

		// our resources
		ID3D11Texture2D *real[2] = {};
		ID3D11ShaderResourceView *real_srv[2] = {};
		ID3D11Texture2D *gen = nullptr;
		ID3D11RenderTargetView *gen_rtv = nullptr;
		ID3D11UnorderedAccessView *gen_uav = nullptr;
		// 0.4.0: where G is built this frame (presenter 1: gen; presenter 2: the job's shared G slot)
		struct gen_target { ID3D11Texture2D *tex = nullptr; ID3D11RenderTargetView *rtv = nullptr; ID3D11UnorderedAccessView *uav = nullptr; } out;
		uint32_t w = 0, h = 0;
		DXGI_FORMAT fmt_tl = DXGI_FORMAT_UNKNOWN, fmt_srgb = DXGI_FORMAT_UNKNOWN, fmt_unorm = DXGI_FORMAT_UNKNOWN;
		bool res_failed_logged = false;
		uint64_t res_retry_frame = 0;          // after a creation failure: no new attempt before this frame
		uint64_t real_frame[2] = { ~0ull, ~0ull };
		ID3D11RenderTargetView *bb_rtv = nullptr;   // _SRGB RTV on the back buffer (Marker barcode on R frames only)
		ID3D11ComputeShader *cs = nullptr;
		ID3D11ComputeShader *diff_cs = nullptr;
		ID3D11ComputeShader *select_cs = nullptr;   // Mode 3: gen = cur after FSR when the cut guard fired
		float last_cut_frac = -1.0f;                 // latest read-back of the cut guard (changed-sample fraction), -1 = none
		uint64_t last_cut_frac_frame = 0;
		ID3D11Buffer *cb = nullptr;
		ID3D11Buffer *stat = nullptr;          // 16-byte raw buffer: [0] = changed samples (GPU cut guard)
		ID3D11UnorderedAccessView *stat_uav = nullptr;
		ID3D11Buffer *stat_stg[4] = {};        // read-back ring for the log (never waited on)
		uint64_t stat_frame[4] = { ~0ull, ~0ull, ~0ull, ~0ull };
		bool cut_guard_ok = false;
		uint32_t cut_logged = 0;
		ID3DDeviceContextState *state = nullptr;
		bool blend_failed = false;

		// queries
		rec_q ring[kRing];
		rec_res res[kRing];
		bool queries_ok = false, queries_failed = false;
		uint64_t bracket = 0;      // id of the open bracket (= frame whose command stream is being recorded)
		uint64_t next_read = 1;

		// frames
		uint64_t frame_rec = 1;    // game frame being recorded
		uint64_t present_k = 0;    // frame whose present event ran last
		bool rp_due = false;       // reshade_present for present_k not seen yet
		frame_flags f, fk;         // current frame / frame present_k (snapshot at its present event)
		uint32_t ps_tag = 0;
		bool active_k = false;     // FG decision for present_k

		// pending R
		bool pending = false;
		bool r_in_bb = false;      // NoWait: R already copied into the back buffer, its present returned WAS_STILL_DRAWING
		int r_kind_first = A_NONE;
		int pending_slot = 0;
		uint64_t pending_frame = 0, pending_gen = 0;

		// state machine
		st_t st = st_t::off;
		const char *reason = "";
		int good_run = 0, need = 60, low_run = 0, high_run = 0, fps_backoff = 0;
		uint64_t active_since_fps_suspend = 0;
		bool occluded = false, err_disabled = false, foreign_presenter = false, exc_disabled = false;
		bool foreign_render = false;           // a present hook rendered through ReShade inside our native Present
		uint64_t err_hold_until = 0;
		int eff_logged = -1;
		// MinBaseFps hysteresis: the FG-off fps is measured while not ACTIVE, the FG-on fps while ACTIVE; resume only when
		// fps_off x ratio (the predicted FG-on fps) clears MinBaseFps + 10 %.
		double fps_on_ema = 0.0, fps_off_ema = 0.0, fg_ratio = 0.8;   // 0.8 until measured: start needs FG-off fps >= 1.375 x MinBaseFps
		bool ratio_measured = false;
		int on_n = 0, off_n = 0;               // samples in the current active / the latest non-active period
		double on_sum = 0.0, fps_off_at_start = 0.0;   // mean FG-on fps over active frames 30..300; FG-off EMA at activation
		int ratio_samples = 0;
		int state_frames = 0;                  // frames since the last state change
		int vsync_mode = -1;                   // videoconfig setting.mat_vsync_mode (-1 = unknown)
		uint64_t vsync_checked_frame = 0;

		// timing
		double last_present_ms = 0.0, fps_ema = 0.0, last_dt = 0.0;
		bool hitch_k = false;                  // present interval of frame k was a hitch (CPU cut signal)
		uint64_t hitch_next = ~0ull;           // 0.3.1: the frame after a hitch is cut too (the big simulation step lands there)
		double t_rp_end = 0.0, t_frame_start = 0.0, t_pre = 0.0;
		UINT orig_latency = 0; bool latency_changed = false;
		int anchor_auto = A_PRE;
		bool bb_bound = false;                 // the last render-target bind included the back buffer (stays across frames)
		uint32_t prev_main_w = 0, prev_main_h = 0;

		// totals
		uint64_t g_total = 0, native_total = 0, late_total = 0, drop_total = 0, foreign_total = 0, cut_cpu_total = 0;
		window_t win;
		// 0.4.0 FSR debug inputs in effect for the latest FSR frame (dump log)
		int dbg_flags = 0; char dbg_phase = '-';
	} g;

	// Work levels. 0 = inert (Enable=0 and Profile=0; or FG OFF after the menu timer started): every command-stream
	// callback returns at once. 1 = only the gameplay flags (main pass / engine_post / TSAA) are collected: needed to start
	// the menu-convar timer at the first gameplay frame. 2 = full (anchors, deadline, GPU timestamps): FG not OFF, or
	// Profile=1. Set at the present event (frame boundary).
	bool g_live = false;   // Enable || Profile (read at start)
	int g_level = 0;
	bool prof_on() { return g_level >= 2; }

	// ------------------------------------------------------------------ menu convar tf2nr_fsr_fg
	namespace menu
	{
		// ICvar::FindVar from the render thread raced Host_Init once (vstdlib crash 2026-09-28 19:08). tf2-fg therefore
		// never calls FindVar before the first GAMEPLAY frame (main pass + engine_post seen: a map is running, Host_Init is
		// long done), waits 20 s more, then looks the convars up at most every 60 frames. If tf2nr_fsr_fg is still missing
		// 60 s later (mod < 0.4.0 or disabled) the lookup stops for good (latched until the next device).
		tf2nr::cvar::var_ref v_fg { "tf2nr_fsr_fg" };
		tf2nr::cvar::var_ref v_nr { "tf2nr_dlss5_nr" };   // our own refs (log only), same names as the bridge's
		tf2nr::cvar::var_ref v_sr { "tf2nr_dlss_sr" };
		uint64_t gameplay_ms = 0, poll = 0, first_try_ms = 0;
		int value = -1;     // -1 = not read yet
		int sr_mode = -1, nr = -1;
		bool logged_missing = false, latched_off = false;

		bool timer_started() { return gameplay_ms != 0; }
		void rearm() { latched_off = false; logged_missing = false; first_try_ms = 0; }

		void update(bool gameplay_frame)
		{
			const uint64_t frame = ++poll;
			const uint64_t now = GetTickCount64();
			if (gameplay_ms == 0)
			{
				if (!gameplay_frame) return;
				gameplay_ms = now;
				logf("menu: first gameplay frame - convar lookup starts in 20 s");
			}
			if (now - gameplay_ms < 20000) return;
			int v = 0;
			if (v_fg.ptr == nullptr)
			{
				if (latched_off) return;
				if (first_try_ms == 0) first_try_ms = now;
				if ((frame % 60) == 1)
				{
					if (tf2nr::cvar::resolve(v_fg)) logf("menu: tf2nr_fsr_fg resolved at %p", v_fg.ptr);
					tf2nr::cvar::resolve(v_nr); tf2nr::cvar::resolve(v_sr);
				}
				if (v_fg.ptr == nullptr)
				{
					if (now - first_try_ms > 60000)
					{
						latched_off = true;
						if (!logged_missing) logf("menu: convar tf2nr_fsr_fg not found after 60 s (TF2NR.GraphicsToggles < 0.4.0?) - lookup stopped, frame generation stays off");
						logged_missing = true;
					}
					return;
				}
			}
			if (v_sr.ptr && tf2nr::cvar::read_int(v_sr, &v)) sr_mode = v;
			if (v_nr.ptr && tf2nr::cvar::read_int(v_nr, &v)) nr = v;
			if (tf2nr::cvar::read_int(v_fg, &v))
			{
				const int n = v != 0 ? 1 : 0;
				if (n != value) logf("menu: tf2nr_fsr_fg = %d (raw %d)", n, v);
				value = n;
			}
			else { first_try_ms = 0; value = -1; logf("menu: tf2nr_fsr_fg could not be read - looking it up again"); }   // read_int cleared ptr
		}
	}

	// ------------------------------------------------------------------ Mode 3: FSR 3 frame interpolation state
	namespace fsr
	{
		tf2fg::Context *ctx = nullptr;
		bool unavailable = false;        // FL < 11_1 (restart required) or unsupported device / too many failures: FG stays OFF
		const char *why = "";
		int creates = 0, failures = 0;
		uint64_t retry_frame = 0;        // no create attempt before this frame (after a failure)
		uint32_t dw = 0, dh = 0;         // display size of ctx
		bool need_reset = true;          // the next dispatch resets FSR history (and presents no G)
		// 0.3.1: the library's own discontinuity rule, mirrored (tf2fg_fsr.cpp dispatch(): no previous dispatch, frameID !=
		// last + 1, HUD-less presence changed -> reset). Without it those reset dispatches were presented and counted as G.
		uint64_t last_disp = ~0ull;      // frameID of the last successful dispatch (~0 = none since the context was created)
		bool last_had_hud = false;
		bool conv_logged = false;        // SR export conventions refused (logged once)
		uint64_t camcut_logged = 0;
		uint64_t sr_gen = ~0ull, hud_gen = ~0ull;
		// camera hold (a frame without CAMERA_VALID reuses the last decoded camera for FsrCamHoldFrames frames)
		float cam_near = 0.0f, cam_far = 0.0f, cam_fov = 0.0f; uint64_t cam_frame = 0; bool cam_ok = false;
		uint64_t cam_logged = 0;
		// exports (re-resolved when the module handle changes: never call through a stale pointer)
		HMODULE sr_mod = nullptr, hud_mod = nullptr;
		PFN_tf2sr_get_fg_inputs get_sr = nullptr;
		PFN_tf2nr_get_hudless get_hud = nullptr;
		bool sr_logged = false, hud_logged = false;
		int sr_miss_run = 0;             // consecutive reshade_present frames without a VALID SR export (Mode 3, FG not OFF)
		uint64_t lib_err_total = 0, lib_warn_total = 0, status_err_total = 0, disp_total = 0, g_total = 0, reset_total = 0;
		uint64_t lib_lines = 0;
		bool dump_hud = false;           // a triplet dump in flight has a HUD-less image
		const char *st_name_last = "";

		void lib_log(void *, tf2fg::LogLevel level, const char *msg)
		{
			if (level == tf2fg::LogLevel::Error) { ++lib_err_total; ++g.win.fsr_lib_err; }
			else if (level == tf2fg::LogLevel::Warn) ++lib_warn_total;
			if (lib_lines++ < 200 || level == tf2fg::LogLevel::Error && lib_err_total <= 1000)
				logf("fsr lib %s: %s", level == tf2fg::LogLevel::Error ? "ERROR" : (level == tf2fg::LogLevel::Warn ? "warn" : "info"), msg ? msg : "");
		}
	}

	bool effective_enable()
	{
		return g_cfg.enable != 0 && menu::value == 1 && !g.err_disabled && !g.exc_disabled && !g.foreign_presenter && !g.foreign_render &&
		       !(g_cfg.mode == 3 && fsr::unavailable);
	}

	// ------------------------------------------------------------------ V-Sync (videoconfig.txt)
	// The native R Present uses SyncInterval (0) while the game's proxied G Present uses the game's own interval. With the
	// in-game V-Sync on, G would wait for vblank and R would not: FG suspends then. TF2 writes the video settings to
	// Documents\Respawn\Titanfall2\local\videoconfig.txt ("setting.mat_vsync_mode" "0" = off) when they are applied.
	int read_vsync_mode()
	{
		PWSTR docs = nullptr;
		if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) || docs == nullptr) { if (docs) CoTaskMemFree(docs); return -1; }
		std::wstring path = docs;
		CoTaskMemFree(docs);
		path += L"\\Respawn\\Titanfall2\\local\\videoconfig.txt";
		FILE *f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
		if (!f) return -1;
		char buf[16384] = {};
		const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		buf[n] = 0;
		const char *key = "\"setting.mat_vsync_mode\"";
		const char *p = strstr(buf, key);
		if (!p) return -1;
		p += strlen(key);
		while (*p == ' ' || *p == '\t') ++p;
		if (*p != '"') return -1;
		++p;
		if (*p < '0' || *p > '9') return -1;
		return atoi(p);
	}

	// ------------------------------------------------------------------ foreign presenters / present hooks
	// Name "module+0xoffset" for a code address (no refcount change).
	std::string module_of(const void *addr)
	{
		HMODULE m = nullptr;
		if (!addr || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(addr), &m) || !m)
		{
			char b[64]; snprintf(b, sizeof(b), "%p (no module)", addr); return b;
		}
		wchar_t path[MAX_PATH] = {};
		GetModuleFileNameW(m, path, MAX_PATH);
		const wchar_t *base = wcsrchr(path, L'\\'); base = base ? base + 1 : path;
		char b[400];
		snprintf(b, sizeof(b), "%ls+0x%llx (%ls)", base, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(m)), path);
		return b;
	}
	// Readable committed memory (VirtualQuery; no fault possible afterwards for n bytes inside one region).
	bool readable(const void *p, size_t n)
	{
		MEMORY_BASIC_INFORMATION mbi = {};
		if (!p || VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
		if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
		const DWORD rd = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		if ((mbi.Protect & rd) == 0) return false;
		const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		return reinterpret_cast<uintptr_t>(p) + n <= end;
	}
	// Where does IDXGISwapChain::Present of the native swap chain go? vtable slot 8, plus one level of inline jmp.
	void log_present_path(IDXGISwapChain *sc)
	{
		if (!sc || !readable(sc, sizeof(void *))) return;
		void *const *vtbl = *reinterpret_cast<void *const *const *>(sc);
		if (!readable(vtbl, sizeof(void *) * 9)) { logf("present path: vtable %p not readable", vtbl); return; }
		const uint8_t *fn = static_cast<const uint8_t *>(vtbl[8]);
		std::string where = module_of(fn), jmp = "none";
		if (readable(fn, 16))
		{
			const void *target = nullptr;
			if (fn[0] == 0xE9) { int32_t rel; memcpy(&rel, fn + 1, 4); target = fn + 5 + rel; }
			else if (fn[0] == 0xFF && fn[1] == 0x25) { int32_t rel; memcpy(&rel, fn + 2, 4); const uint8_t *slot = fn + 6 + rel; if (readable(slot, 8)) memcpy(&target, slot, 8); }
			else if (fn[0] == 0x48 && fn[1] == 0xB8 && fn[10] == 0xFF && fn[11] == 0xE0) memcpy(&target, fn + 2, 8);
			if (target) jmp = module_of(target);
			char b[80]; snprintf(b, sizeof(b), " | first bytes %02x %02x %02x %02x %02x %02x", fn[0], fn[1], fn[2], fn[3], fn[4], fn[5]);
			jmp += b;
		}
		logf("present path: native IDXGISwapChain::Present (vtable slot 8) -> %s | inline jump -> %s", where.c_str(), jmp.c_str());
	}

	namespace xa { std::string hook_mods; }   // 0.4.1: presenter 2's view of the listed overlay DLLs (xa_presenter.hpp)
	void scan_modules(bool first)
	{
		static std::string last;
		if (GetModuleHandleW(L"NvPresent64.dll") && !g.foreign_presenter)
		{
			g.foreign_presenter = true;
			logf("NvPresent64.dll (NVIDIA Smooth Motion) is loaded: the 'native' swap chain would be NVIDIA's proxy - frame generation stays OFF");
		}
		const wchar_t *hooks[] = { L"GameOverlayRenderer64.dll", L"RTSSHooks64.dll", L"DiscordHook64.dll", L"obs-vulkan64.dll", L"graphics-hook64.dll",
		                           L"nvspcap64.dll", L"NvPresent64.dll", L"overlay64.dll", L"igo64.dll" };
		std::string found, p2;
		for (const wchar_t *m : hooks)
			if (GetModuleHandleW(m))
			{
				char b[64]; snprintf(b, sizeof(b), "%s%ls", found.empty() ? "" : ", ", m); found += b;
				// 0.4.1: presenter 2 - every listed hook DLL except the two loaded in every tested run (nvspcap64 = NVIDIA capture,
				// igo64 = the EA app's in-game overlay, which the game's EA login brings along; dxgi.dll's swap-chain Present stayed
				// unpatched with both, and the composition chain's own code is checked when it is created and every 600 frames)
				// and NvPresent64 (FG stays off anyway)
				if (_wcsicmp(m, L"nvspcap64.dll") != 0 && _wcsicmp(m, L"igo64.dll") != 0 && _wcsicmp(m, L"NvPresent64.dll") != 0) { snprintf(b, sizeof(b), "%s%ls", p2.empty() ? "" : ", ", m); p2 += b; }
			}
		if (first || found != last)
			logf("present-hook modules loaded: %s%s%s", found.empty() ? "none" : found.c_str(),
				found.empty() ? "" : " (events they raise inside our native Present are counted; any switches FG off unless AllowPresentHooks=1)",
				p2.empty() ? "" : (g_cfg.allow_present_hooks ? " | presenter 2: overlay hooks accepted (AllowPresentHooks=1)" : " | presenter 2 NOT eligible while they are loaded (their Present hook would run on the pacer thread; AllowPresentHooks=1 overrides)"));
		last = found;
		xa::hook_mods = p2;
	}

	// ------------------------------------------------------------------ presenter 2 (xa + DComp), 0.4.0
#include "xa_presenter.hpp"

	void xa_sync_cfg()
	{
		xa::cfg_presenter = g_cfg.presenter;
		xa::cfg_spin_us = g_cfg.xa_spin_us;
		xa::cfg_device_mode = g_cfg.xa_device;
		xa::cfg_present_log = g_cfg.present_log;
		xa::cfg_log_window = g_cfg.xa_log_window;
		xa::cfg_allow_hooks = g_cfg.allow_present_hooks;
		xa::cfg_test_build_teardown = g_cfg.xa_test_build_teardown;
		xa::cfg_test_stall_ms = g_cfg.xa_test_stall_ms;
		xa::cfg_test_stall_at = g_cfg.xa_test_stall_at;
		xa::cfg_off_clamp_ms = static_cast<double>(g_cfg.xa_off_clamp_ms);
		xa::cfg_g_hold = g_cfg.xa_g_hold;
		xa::cfg_g_hold_ab = g_cfg.xa_g_hold_ab;
	}

	// The presenter FG uses now: 0 = pass-through (FG suspended, no G shown), 1 = in-queue (M1), 2 = xa + DComp.
	// 0.4.1: listed present-hook overlays (other than nvspcap64, loaded in every tested run) keep presenter 2 off unless
	// AllowPresentHooks=1 - their Present hook would run on the pacer thread for the composition chain
	bool xa_hooks_ok() { return xa::cfg_allow_hooks || (xa::hook_mods.empty() && !xa::chain_hooked); }
	int presenter_now()
	{
		if (g_cfg.presenter == 1) return 1;
		if (g_cfg.presenter == 0) return 0;
		const bool xa_ok = xa::topo.known && xa::topo.eligible && !xa::disabled_why && !xa::D.failed && !xa::abandoned && xa_hooks_ok();
		if (g_cfg.presenter == 2) return xa_ok ? 2 : 0;
		if (!xa::topo.known) return 0;           // unknown topology: never guess
		// 0.4.1: the display is on the render adapter (MUX dGPU mode / external monitor on the RTX): pass-through. Presenter 1
		// (in-queue) was never validated on such an output and showed 12-40 % of G on this laptop (fewer frames than FG off);
		// Presenter=1 still selects it explicitly.
		if (xa::topo.same_adapter) return 0;
		return xa_ok ? 2 : 0;                    // hybrid: presenter 2, else pass-through
	}
	const char *pass_reason()
	{
		static char buf[320];
		if (g_cfg.presenter == 0) return "no presenter: Presenter=0 (pass-through)";
		if (!xa::topo.known) return "no presenter: display topology unknown";
		if (xa::abandoned || xa::disabled_why) return "no presenter: presenter 2 failed this session (see log)";
		if (xa::D.failed) return "no presenter: presenter 2 devices could not be created (see log)";
		if (g_cfg.presenter < 0 && xa::topo.same_adapter) return "no presenter: the display is driven by the render adapter - presenter 1 is not validated there (Presenter=1 forces it)";
		if (!xa::topo.eligible) return "no presenter: presenter 2 not possible on this topology (see log)";
		if (!xa_hooks_ok())
		{
			snprintf(buf, sizeof(buf), "no presenter: %s%s (AllowPresentHooks=1 accepts it)", xa::chain_hooked ? xa::hook_why : "present-hook overlay loaded: ", xa::chain_hooked ? "" : xa::hook_mods.c_str());
			return buf;
		}
		return "no presenter";
	}
	bool xa_minimized = false;

	bool xa_ready()
	{
		return !xa::abandoned && !xa::stalling && !xa::builder && xa::S.ok && xa::thread != nullptr && xa::X.running.load() && xa::X.error.load() == 0 &&
		       xa::S.w == g.w && xa::S.h == g.h && xa::S.hwnd == xa::topo.hwnd && !xa_minimized;
	}
	const char *xa_not_ready_reason()
	{
		if (xa_minimized) return "window minimized";
		if (xa::stalling) return "presenter 2: pacer stalled (stopping; rebuilt after it exits)";
		if (xa::builder) return "presenter 2: devices / surfaces being created (builder thread)";
		if (!xa::D.ok) return "presenter 2: devices not created yet";
		if (!xa::S.ok) return xa::S.failed ? "presenter 2: surfaces failed (retry in 600 frames)" : "presenter 2: surfaces not created yet";
		if (!xa::thread || !xa::X.running.load()) return "presenter 2: pacer not running yet";
		return "presenter 2 not ready";
	}

	// Render thread, outside ACTIVE (WARMUP / SUSPENDED): devices (once) + surfaces (per size / window) on the builder
	// thread (0.4.0 m4d: no render-thread hitch), then the pacer.
	bool xa_ensure()
	{
		if (xa::abandoned || xa::disabled_why || xa::stalling || !xa::topo.eligible || !xa_hooks_ok() || !g.dev || !g.imm || g.w == 0 || g.h == 0) return false;
		xa::poll_builder();   // takes a finished build / runs a deferred teardown
		if (xa::builder || xa::pend_level) return false;   // still building (or a teardown still pending)
		if (xa::S.ok && (xa::S.w != g.w || xa::S.h != g.h || xa::S.hwnd != xa::topo.hwnd || xa::S.sc_gen != g.sc_gen)) xa::teardown(1, "back buffer size / window / swap chain changed");
		// 0.4.1: devices of another adapter pair (monitor / adapter change while they lived): rebuild them
		if (xa::D.ok && (!xa::luid_eq(xa::D.render_luid, xa::topo.render) || !xa::luid_eq(xa::D.display_luid, xa::topo.display))) xa::teardown(2, "render / display adapter changed");
		if (xa::abandoned || xa::D.failed || xa::builder) return false;
		if (!xa::S.ok)
		{
			if (xa::S.failed && g.frame_rec < xa::S.fail_frame + 600) return false;
			if (g.dev_flags & D3D11_CREATE_DEVICE_SINGLETHREADED)
			{
				// a single-threaded device must not be used from another thread: everything here (hitch, but correct)
				xa::slot_handles hs;
				if (!xa::D.ok && !xa::create_devices(xa::topo.render, xa::topo.display)) return false;
				if (!xa::create_surfaces_d3d11(g.dev, g.imm, g.w, g.h, g.fmt_tl, g.fmt_srgb, g.fmt_unorm, g.frame_rec, hs)) return false;
				const bool ok = xa::create_surfaces_x(g.w, g.h, xa::topo.hwnd, g.sc_gen, g.frame_rec, hs, nullptr);
				xa::close_handles(hs);
				if (!ok) { xa::release_surfaces(); return false; }
				xa::reset_jobs();
			}
			else
			{
				xa::start_builder(g.dev, g.imm, g.w, g.h, g.fmt_tl, g.fmt_srgb, g.fmt_unorm, xa::topo.hwnd, g.sc_gen, g.frame_rec);
				return false;
			}
		}
		if (!xa::thread) xa::start_pacer(g_cfg.pace_bias_ms);
		return xa::thread != nullptr;
	}

	// ------------------------------------------------------------------ 0.4.1 measurement helpers
	// CPU split between this (game) process and the whole machine (all logical processors), between two samples.
	struct cpu_snap { uint64_t proc = 0, sys_busy = 0, sys_total = 0; };
	uint64_t ft64(const FILETIME &f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }
	cpu_snap cpu_now()
	{
		cpu_snap c;
		FILETIME cr, ex, k, u;
		if (GetProcessTimes(GetCurrentProcess(), &cr, &ex, &k, &u)) c.proc = ft64(k) + ft64(u);
		FILETIME id, sk, su;
		if (GetSystemTimes(&id, &sk, &su)) { c.sys_total = ft64(sk) + ft64(su); c.sys_busy = c.sys_total - ft64(id); }
		return c;
	}
	void cpu_split(const cpu_snap &a, const cpu_snap &b, double *game_pct, double *sys_pct)
	{
		const double tot = b.sys_total > a.sys_total ? static_cast<double>(b.sys_total - a.sys_total) : 0.0;
		*game_pct = tot > 0.0 ? 100.0 * static_cast<double>(b.proc - a.proc) / tot : NAN;
		*sys_pct = tot > 0.0 ? 100.0 * static_cast<double>(b.sys_busy - a.sys_busy) / tot : NAN;
	}

	// XaCostAB=N (measurement): phases of N seconds from the first ACTIVE frame on, cycling 0 OFF (forced SUSPENDED: no FSR,
	// no presenter; FSR context, HUD-less copies and - with Profile=1 - work level 2 stay) -> 1 FSR only (G built every frame,
	// never presented; the pacer idles) -> 2 FSR + presenter 2. Per phase (first 3 s skipped): fps, frame-time p50/p95 and the
	// game process's CPU vs the machine's.
	namespace costab
	{
		double t0 = 0.0, ph_t0 = 0.0, acc_t0 = 0.0;
		int phase = -1;
		bool acc = false;
		uint64_t frames = 0, active = 0;
		cpu_snap c0;
		std::vector<float> ft;
		const char *name(int p) { return p == 0 ? "OFF (suspended)" : (p == 1 ? "FSR only (no presenter)" : "FSR + presenter 2"); }
		float pctv(std::vector<float> v, double q)
		{
			if (v.empty()) return NAN;
			const size_t i = std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size() - 1) + 0.5));
			std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(i), v.end());
			return v[i];
		}
		void summary(double now)
		{
			if (!acc || frames == 0) return;
			double gp = NAN, sp = NAN; cpu_split(c0, cpu_now(), &gp, &sp);
			const double secs = (now - acc_t0) / 1000.0;
			logf("cost A/B phase %s: %.1f s, %llu frames = %.2f fps (frame ms p50 %.2f p95 %.2f), ACTIVE frames %.0f %% | CPU: game process %.1f %% of the machine, whole system %.1f %% (rest %.1f %%)",
				name(phase), secs, static_cast<unsigned long long>(frames), secs > 0.0 ? static_cast<double>(frames) / secs : 0.0, pctv(ft, 0.5), pctv(ft, 0.95),
				100.0 * static_cast<double>(active) / static_cast<double>(frames), gp, sp, sp - gp);
		}
		// render thread (on_present, after update_state)
		void tick(double now, bool is_active, double dt)
		{
			if (g_cfg.cost_ab <= 0) return;
			if (t0 <= 0.0) { if (!is_active) return; t0 = now; }
			const int p = static_cast<int>(std::floor((now - t0) / (static_cast<double>(g_cfg.cost_ab) * 1000.0))) % 3;
			if (p != phase)
			{
				summary(now);
				phase = p; ph_t0 = now; acc = false; frames = active = 0; ft.clear();
				logf("cost A/B: phase -> %s (XaCostAB=%d s)", name(p), g_cfg.cost_ab);
				return;
			}
			if (!acc) { if (now - ph_t0 >= 3000.0) { acc = true; acc_t0 = now; c0 = cpu_now(); frames = active = 0; ft.clear(); } return; }
			++frames; if (is_active) ++active;
			if (dt > 0.0 && dt < 1000.0) ft.push_back(static_cast<float>(dt));
		}
	}

	// tf2-fg-frames.csv (PresentLog=1, i.e. with the Marker=2 barcode): one F row per game frame from the render thread -
	// F,seq_r,frame,t_rp_end_ms,job(0 = no job: the game's own present shows R(k)),state. Joined with dupmon's barcode reads it
	// gives "end of our reshade_present(k) -> R(k) first on screen" for FG on AND off in the same run.
	namespace fcsv
	{
		FILE *f = nullptr;
		std::string buf;
		bool failed = false;
		void row(uint32_t seq_r, uint64_t frame, double t, uint64_t job, int st)
		{
			if (failed) return;
			char b[96]; snprintf(b, sizeof(b), "F,%u,%llu,%.3f,%llu,%d\n", seq_r, static_cast<unsigned long long>(frame), t, static_cast<unsigned long long>(job), st);
			buf += b;
		}
		void flush()
		{
			if (buf.empty() || failed) return;
			if (!f)
			{
				f = _wfsopen((g_dir / "tf2-fg-frames.csv").c_str(), L"ab", _SH_DENYWR);
				if (!f) { failed = true; buf.clear(); logf("frames csv: could not open tf2-fg-frames.csv"); return; }
				fputs("# F,seq_r,frame,t_rp_end_ms,job,state(0 off 1 warmup 2 active 3 suspended)\n", f);
			}
			fwrite(buf.data(), 1, buf.size(), f); fflush(f); buf.clear();
		}
	}

	// ------------------------------------------------------------------ resources
	void release_resources()
	{
		for (int i = 0; i < 2; ++i) { safe_release(g.real_srv[i]); safe_release(g.real[i]); g.real_frame[i] = ~0ull; }
		safe_release(g.gen_uav); safe_release(g.gen_rtv); safe_release(g.gen);
		safe_release(g.bb_rtv);
		g.w = g.h = 0; g.fmt_tl = g.fmt_srgb = g.fmt_unorm = DXGI_FORMAT_UNKNOWN;
	}

	bool format_family(DXGI_FORMAT f, DXGI_FORMAT &tl, DXGI_FORMAT &srgb, DXGI_FORMAT &unorm)
	{
		switch (f)
		{
		case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			tl = DXGI_FORMAT_R8G8B8A8_TYPELESS; srgb = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; unorm = DXGI_FORMAT_R8G8B8A8_UNORM; return true;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			tl = DXGI_FORMAT_B8G8R8A8_TYPELESS; srgb = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; unorm = DXGI_FORMAT_B8G8R8A8_UNORM; return true;
		default:
			return false;
		}
	}

	// The back buffer's desc; false when it cannot be copied to/from our textures.
	bool bb_desc(D3D11_TEXTURE2D_DESC &d)
	{
		if (g.bb == 0) return false;
		auto *tex = reinterpret_cast<ID3D11Texture2D *>(g.bb);
		tex->GetDesc(&d);
		return d.Width > 0 && d.Height > 0 && d.Width <= 16384 && d.Height <= 16384 && d.MipLevels == 1 && d.ArraySize == 1 && d.SampleDesc.Count == 1;
	}

	bool ensure_resources()
	{
		if (!g.dev) return false;
		D3D11_TEXTURE2D_DESC bd = {};
		if (!bb_desc(bd)) return false;
		DXGI_FORMAT tl, srgb, unorm;
		if (!format_family(bd.Format, tl, srgb, unorm))
		{
			if (!g.res_failed_logged) logf("back buffer format %u is not RGBA8/BGRA8 - frame generation unavailable", static_cast<unsigned>(bd.Format));
			g.res_failed_logged = true;
			return false;
		}
		if (g.real[0] && g.real[1] && g.gen && g.w == bd.Width && g.h == bd.Height && g.fmt_tl == tl) return true;
		if (g.frame_rec < g.res_retry_frame) return false;   // a creation failure is latched: no per-frame allocation churn
		release_resources();
		D3D11_TEXTURE2D_DESC d = {};
		d.Width = bd.Width; d.Height = bd.Height; d.MipLevels = 1; d.ArraySize = 1; d.Format = tl; d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bool ok = true;
		for (int i = 0; i < 2 && ok; ++i)
		{
			ok = SUCCEEDED(g.dev->CreateTexture2D(&d, nullptr, &g.real[i])) && g.real[i] != nullptr;
			if (ok)
			{
				D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
				sd.Format = srgb; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
				ok = SUCCEEDED(g.dev->CreateShaderResourceView(g.real[i], &sd, &g.real_srv[i])) && g.real_srv[i] != nullptr;
			}
		}
		if (ok)
		{
			d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
			if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &g.gen)) || g.gen == nullptr)
			{
				// no typed-UAV support for this format: blend unavailable, duplicate mode still works
				d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
				g.gen = nullptr;
				ok = SUCCEEDED(g.dev->CreateTexture2D(&d, nullptr, &g.gen)) && g.gen != nullptr;
			}
		}
		if (ok)
		{
			D3D11_RENDER_TARGET_VIEW_DESC rd = {};
			rd.Format = srgb; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			ok = SUCCEEDED(g.dev->CreateRenderTargetView(g.gen, &rd, &g.gen_rtv)) && g.gen_rtv != nullptr;
		}
		if (ok)
		{
			D3D11_TEXTURE2D_DESC gd = {}; g.gen->GetDesc(&gd);
			if (gd.BindFlags & D3D11_BIND_UNORDERED_ACCESS)
			{
				D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
				ud.Format = unorm; ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
				if (FAILED(g.dev->CreateUnorderedAccessView(g.gen, &ud, &g.gen_uav))) g.gen_uav = nullptr;
			}
		}
		if (!ok)
		{
			release_resources();
			if (!g.res_failed_logged) logf("resources: creating %ux%u textures FAILED - frame generation unavailable (next attempt in 600 frames or at the next swap chain)", bd.Width, bd.Height);
			g.res_failed_logged = true;
			g.res_retry_frame = g.frame_rec + 600;
			return false;
		}
		g.w = bd.Width; g.h = bd.Height; g.fmt_tl = tl; g.fmt_srgb = srgb; g.fmt_unorm = unorm;
		logf("resources: real[2] + gen %ux%u (typeless fmt %u, SRV/RTV fmt %u, UAV %s), back buffer fmt %u",
			g.w, g.h, static_cast<unsigned>(tl), static_cast<unsigned>(srgb), g.gen_uav ? "ok" : "unavailable (blend disabled)", static_cast<unsigned>(bd.Format));
		return true;
	}

	bool ensure_blend()
	{
		if (g.blend_failed) return false;
		if (g.cs && g.cb && (g_cfg.state_mode == 1 || g.state)) return true;
		if (!g.dev || !g.gen_uav) { g.blend_failed = true; return false; }
		bool ok = SUCCEEDED(g.dev->CreateComputeShader(g_blend_cs, sizeof(g_blend_cs), nullptr, &g.cs)) && g.cs != nullptr;
		if (ok) ok = SUCCEEDED(g.dev->CreateComputeShader(g_select_cs, sizeof(g_select_cs), nullptr, &g.select_cs)) && g.select_cs != nullptr;
		if (ok)
		{
			D3D11_BUFFER_DESC bd = { 32, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
			ok = SUCCEEDED(g.dev->CreateBuffer(&bd, nullptr, &g.cb)) && g.cb != nullptr;
		}
		if (ok && g_cfg.state_mode == 0)
		{
			if (!g.dev1 || !g.imm1)
			{
				logf("blend: ID3D11Device1/ID3D11DeviceContext1 unavailable - manual CS save/restore (StateMode=1)");
				g_cfg.state_mode = 1;
			}
			else
			{
				// Private context state at the device's OWN feature level (a higher level would raise the device's level,
				// verifier M1b note) and matching creation flags.
				const D3D_FEATURE_LEVEL fl = g.fl;
				D3D_FEATURE_LEVEL chosen = static_cast<D3D_FEATURE_LEVEL>(0);
				const UINT flags = (g.dev_flags & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
				const HRESULT hr = g.dev1->CreateDeviceContextState(flags, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), &chosen, &g.state);
				if (FAILED(hr) || g.state == nullptr)
				{
					logf("blend: CreateDeviceContextState(level 0x%x, flags 0x%x) FAILED 0x%08x - manual CS save/restore (StateMode=1)", static_cast<unsigned>(fl), flags, static_cast<unsigned>(hr));
					g.state = nullptr;
					g_cfg.state_mode = 1;
				}
				else logf("blend: private context state created at level 0x%x (flags 0x%x)", static_cast<unsigned>(chosen), flags);
			}
		}
		if (!ok)
		{
			logf("blend: compute shader / constant buffer creation FAILED - Mode 2 falls back to duplicate frames, Mode 3 runs without the GPU cut guard");
			safe_release(g.cs); safe_release(g.select_cs); safe_release(g.cb);
			g.blend_failed = true;
			return false;
		}
		// GPU cut guard: diff CS + a 16-byte raw UAV counter + a staging ring for the log. Optional: without it the blend
		// runs unguarded (the CPU cut checks still apply).
		g.cut_guard_ok = false;
		if (g_cfg.cut_percent > 0.0f && g.imm1)
		{
			bool gk = SUCCEEDED(g.dev->CreateComputeShader(g_diff_cs, sizeof(g_diff_cs), nullptr, &g.diff_cs)) && g.diff_cs != nullptr;
			if (gk)
			{
				D3D11_BUFFER_DESC sd = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0 };
				gk = SUCCEEDED(g.dev->CreateBuffer(&sd, nullptr, &g.stat)) && g.stat != nullptr;
			}
			if (gk)
			{
				D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
				ud.Format = DXGI_FORMAT_R32_TYPELESS; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
				ud.Buffer.FirstElement = 0; ud.Buffer.NumElements = 4; ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
				gk = SUCCEEDED(g.dev->CreateUnorderedAccessView(g.stat, &ud, &g.stat_uav)) && g.stat_uav != nullptr;
			}
			for (int i = 0; i < 4 && gk; ++i)
			{
				D3D11_BUFFER_DESC td = { 16, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0 };
				gk = SUCCEEDED(g.dev->CreateBuffer(&td, nullptr, &g.stat_stg[i])) && g.stat_stg[i] != nullptr;
				g.stat_frame[i] = ~0ull;
			}
			if (!gk)
			{
				logf("blend: cut guard resources FAILED - the blend runs without the GPU cut guard");
				safe_release(g.diff_cs); safe_release(g.stat_uav); safe_release(g.stat);
				for (auto *&b : g.stat_stg) safe_release(b);
			}
			else
			{
				g.cut_guard_ok = true;
				logf("blend: GPU cut guard on (gen = cur when > %.1f %% of 1/64 sampled pixels change by > %.3f perceptual luma)", g_cfg.cut_percent, g_cfg.cut_delta);
			}
		}
		return true;
	}

	void release_blend()
	{
		safe_release(g.state); safe_release(g.cs); safe_release(g.select_cs); safe_release(g.cb);
		safe_release(g.diff_cs); safe_release(g.stat_uav); safe_release(g.stat);
		for (auto *&b : g.stat_stg) safe_release(b);
		for (auto &f : g.stat_frame) f = ~0ull;
		g.cut_guard_ok = false;
		g.blend_failed = false;
	}

	// D3D11 predication also applies to copies/clears/dispatches: lift a predicate the game may have set around our own
	// commands and put it back afterwards.
	struct pred_guard
	{
		ID3D11Predicate *p = nullptr; BOOL v = FALSE; ID3D11DeviceContext *c = nullptr;
		explicit pred_guard(ID3D11DeviceContext *ctx) : c(ctx) { if (c) { c->GetPredication(&p, &v); if (p) c->SetPredication(nullptr, FALSE); } }
		~pred_guard() { if (c && p) { c->SetPredication(p, v); p->Release(); } }
		pred_guard(const pred_guard &) = delete; pred_guard &operator=(const pred_guard &) = delete;
	};

	// ------------------------------------------------------------------ timestamps
	bool ensure_queries()
	{
		if (g.queries_ok) return true;
		if (g.queries_failed || !g.dev) return false;
		D3D11_QUERY_DESC dj = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, ts = { D3D11_QUERY_TIMESTAMP, 0 };
		bool ok = true;
		for (int i = 0; i < kRing && ok; ++i)
		{
			ok = SUCCEEDED(g.dev->CreateQuery(&dj, &g.ring[i].dj)) && g.ring[i].dj;
			for (int k = 0; k < TS_COUNT && ok; ++k) ok = SUCCEEDED(g.dev->CreateQuery(&ts, &g.ring[i].ts[k])) && g.ring[i].ts[k];
		}
		if (!ok)
		{
			for (auto &r : g.ring) { safe_release(r.dj); for (auto *&q : r.ts) safe_release(q); r = rec_q {}; }
			logf("queries: creation FAILED - no GPU timing");
			g.queries_failed = true;
			return false;
		}
		g.queries_ok = true;
		return true;
	}

	void release_queries()
	{
		for (auto &r : g.ring) { safe_release(r.dj); for (auto *&q : r.ts) safe_release(q); r = rec_q {}; }
		for (auto &r : g.res) r = rec_res {};
		g.queries_ok = false;
		g.bracket = 0;
	}

	rec_q *open_rec()
	{
		if (!g.queries_ok || g.bracket == 0) return nullptr;
		rec_q &r = g.ring[g.bracket % kRing];
		return (r.open && r.id == g.bracket) ? &r : nullptr;
	}

	void ts(int kind)
	{
		if (!prof_on() || !g.imm) return;
		rec_q *r = open_rec();
		if (!r || (r->issued & (1u << kind)) || !r->ts[kind]) return;
		g.imm->End(r->ts[kind]);
		r->issued |= 1u << kind;
	}

	// Close bracket k (the frame whose present event is running). Always, whatever the work level, so no disjoint query
	// is left begun when timing stops.
	void close_bracket(uint64_t k)
	{
		if (!g.queries_ok || !g.imm || g.bracket != k) return;
		rec_q &c = g.ring[k % kRing];
		if (c.open && c.id == k)
		{
			if (c.ts[TS_E]) { g.imm->End(c.ts[TS_E]); c.issued |= 1u << TS_E; }
			g.imm->End(c.dj);
			c.open = false; c.pending_read = true;
		}
	}
	// Open bracket n (= k+1) - only at work level 2 (FG not OFF, or Profile=1).
	void open_bracket(uint64_t n)
	{
		if (!g.imm || !ensure_queries()) return;
		rec_q &r = g.ring[n % kRing];
		if (r.open) { g.imm->End(r.dj); r.open = false; }   // never expected (ring wrapped without a close)
		if (r.pending_read) { r.pending_read = false; ++g.win.lost; }
		r.id = n; r.issued = 0; r.r_kind = A_NONE; r.late = false; r.g_active = false;
		g.imm->Begin(r.dj);
		r.open = true;
		g.bracket = n;
	}

	float ms_between(const rec_res &a, int ka, const rec_res &b, int kb)
	{
		if (!(a.mask & (1u << ka)) || !(b.mask & (1u << kb)) || a.freq == 0) return NAN;
		const double d = static_cast<double>(static_cast<int64_t>(b.ts[kb] - a.ts[ka]));
		const double ms = d * 1000.0 / static_cast<double>(a.freq);
		return (ms >= 0.0 && ms < 1000.0) ? static_cast<float>(ms) : NAN;
	}
	void push(std::vector<float> &v, float x) { if (std::isfinite(x) && v.size() < 100000) v.push_back(x); }
	void push_cpu(std::vector<float> &v, double from, double to) { if (from > 0.0 && to >= from && to - from < 1000.0) push(v, static_cast<float>(to - from)); }

	// a = bracket id-1, b = bracket id (both resolved)
	void process_pair(const rec_res &a, const rec_res &b)
	{
		if (!a.ok || a.disjoint) return;
		++g.win.samples;
		const bool b_ok = b.ok && !b.disjoint && b.freq == a.freq && b.id == a.id + 1;
		if (a.r_kind != A_NONE)
		{
			push(g.win.g2r, ms_between(a, TS_G, a, TS_R));
			if (b_ok && b.g_active) push(g.win.r2g, ms_between(a, TS_R, b, TS_G));
		}
		if (b_ok) push(g.win.d, ms_between(a, TS_G, b, TS_G));
		push(g.win.pre, ms_between(a, TS_G, a, TS_PRE));
		push(g.win.post, ms_between(a, TS_G, a, TS_POST));
		push(g.win.dead, ms_between(a, TS_G, a, TS_DEAD));
		push(g.win.chunk, ms_between(a, TS_PRE, a, TS_POST));
		push(g.win.fp, ms_between(a, TS_G, a, TS_FP));
		push(g.win.send, ms_between(a, TS_G, a, TS_E));
		if (a.g_active) push(g.win.gbuild, ms_between(a, TS_G0, a, TS_G));
	}

	void read_back()
	{
		if (!g.queries_ok || !g.imm) return;
		if (g.bracket > static_cast<uint64_t>(kRing) && g.next_read + kRing < g.bracket) g.next_read = g.bracket - kRing;   // after a gap without timing
		while (g.next_read + 2 < g.bracket)
		{
			const uint64_t id = g.next_read;
			rec_q &r = g.ring[id % kRing];
			if (!r.pending_read || r.id != id) { ++g.next_read; continue; }
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
			const HRESULT hr = g.imm->GetData(r.dj, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH);
			if (hr == S_FALSE)
			{
				if (g.bracket - id >= static_cast<uint64_t>(kRing - 3)) { r.pending_read = false; ++g.win.lost; ++g.next_read; continue; }
				break;   // not ready yet: try again next frame
			}
			rec_res &o = g.res[id % kRing];
			o = rec_res {};
			o.id = id; o.r_kind = r.r_kind; o.late = r.late; o.g_active = r.g_active;
			r.pending_read = false;
			++g.next_read;
			if (FAILED(hr)) continue;
			o.ok = true; o.disjoint = dj.Disjoint != FALSE; o.freq = dj.Frequency;
			if (o.disjoint) ++g.win.disjoint;
			for (int k = 0; k < TS_COUNT; ++k)
			{
				if (!(r.issued & (1u << k)) || !r.ts[k]) continue;
				UINT64 v = 0;
				if (g.imm->GetData(r.ts[k], &v, sizeof(v), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) { o.ts[k] = v; o.mask |= 1u << k; }
			}
			const rec_res &prev = g.res[(id - 1) % kRing];
			if (prev.id == id - 1) process_pair(prev, o);
		}
	}

	// ------------------------------------------------------------------ native present
	void handle_hr(HRESULT hr, const char *what)
	{
		if (hr == S_OK) return;
		if (hr == DXGI_STATUS_OCCLUDED)
		{
			if (!g.occluded) logf("%s: DXGI_STATUS_OCCLUDED - extra presents paused while occluded", what);
			g.occluded = true; ++g.win.occluded;
			return;
		}
		if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
		{
			HRESULT reason = g.dev ? g.dev->GetDeviceRemovedReason() : E_FAIL;
			logf("%s: 0x%08x (device removed/reset, reason 0x%08x) - frame generation OFF for this session", what, static_cast<unsigned>(hr), static_cast<unsigned>(reason));
			g.err_disabled = true; ++g.win.dxgi_err;
			return;
		}
		++g.win.dxgi_err;
		static int logged = 0;
		if (logged++ < 10) logf("%s: unexpected result 0x%08x - suspended for 300 frames", what, static_cast<unsigned>(hr));
		if (FAILED(hr)) g.err_hold_until = g.frame_rec + 300;
	}

	HRESULT native_present(UINT flags)
	{
		IDXGISwapChain *sc = g.native_sc;
		if (!sc) return E_POINTER;
		const double a = qpc_ms();
		const uint32_t foreign0 = t_foreign;
		t_in_native = true;
		HRESULT hr = E_FAIL;
		try { hr = sc->Present(static_cast<UINT>(g_cfg.sync_interval), flags); }
		catch (...) { hr = E_FAIL; }
		t_in_native = false;
		const double dt = qpc_ms() - a;
		const uint32_t foreign = t_foreign - foreign0;
		if (foreign != 0)
		{
			// A present hook rendered through ReShade's proxy device inside our mid-frame Present: tf2-dlss-sr and
			// tf2-nr-bridge received those events too (they have no guard) and may have lost their per-frame pass state.
			g.win.foreign += foreign; g.foreign_total += foreign;
			if (!g_cfg.allow_present_hooks && !g.foreign_render)
			{
				g.foreign_render = true;
				logf("a present hook raised %u ReShade events inside our native Present (it renders through ReShade's proxy device) - frame generation OFF for this session (AllowPresentHooks=1 overrides)", foreign);
			}
			else if (g.foreign_total == foreign) logf("a present hook raised %u ReShade events inside our native Present (AllowPresentHooks=1: FG stays on)", foreign);
		}
		if ((flags & DXGI_PRESENT_TEST) == 0)
		{
			push(g.win.c_rpres, static_cast<float>(dt));
			g.win.present_cpu_ms += dt;
			if (dt > g.win.present_cpu_max) g.win.present_cpu_max = dt;
			++g.win.native_presents; ++g.native_total;
		}
		return hr;
	}

	bool pending_valid()
	{
		return g.pending && g.pending_gen == g.sc_gen && g.native_sc && g.bb && g.imm && g.pending_slot >= 0 && g.pending_slot < 2 &&
		       g.real[g.pending_slot] && g.real_frame[g.pending_slot] == g.pending_frame;
	}

	ID3D11RenderTargetView *bb_rtv();                                              // Marker=2 (defined below)
	void stamp_barcode(ID3D11RenderTargetView *rtv, bool is_r, uint64_t frame);

	// Present R(pending) now: copy it into the back buffer, then a native Present. Called on the immediate context's thread.
	// NoWait=1: the present at an anchor uses DXGI_PRESENT_DO_NOT_WAIT; if DXGI would block (WAS_STILL_DRAWING) R stays in
	// the back buffer and pending, and the present is retried at later render-target binds / anchors. The deadline and
	// the late path always present blocking (R must be out before the game writes the back buffer).
	void inject_r(int kind)
	{
		if (!g.pending) return;
		if (!pending_valid() || g.occluded)
		{
			g.pending = false; g.r_in_bb = false; ++g.win.r_dropped; ++g.drop_total;
			return;
		}
		if (!g.r_in_bb)
		{
			ID3D11Resource *bb = reinterpret_cast<ID3D11Resource *>(g.bb);
			{
				pred_guard pg(g.imm);
				g.imm->CopyResource(bb, g.real[g.pending_slot]);
			}
			if (g_cfg.marker & 2) stamp_barcode(bb_rtv(), true, g.pending_frame);
			ts(kind == A_LATE ? TS_RL : TS_R);
			g.r_kind_first = kind;
		}
		if (g_cfg.debug_skip_r)
		{
			g.pending = false; g.r_in_bb = false;
			// measurement modes: 1 = no present, 2 = Flush() in place of Present (CPU cost of submitting the partial frame).
			// Neither is a present: they are counted as "flushes", not in presents per game frame.
			if (g_cfg.debug_skip_r == 2)
			{
				const double a = qpc_ms();
				g.imm->Flush();
				const double dt = qpc_ms() - a;
				push(g.win.c_rpres, static_cast<float>(dt));
				++g.win.flushes;
			}
			return;   // not a present: no R counters (the window line shows flushes/skips separately)
		}
		const bool may_wait = !(g_cfg.no_wait && kind != A_DEAD && kind != A_LATE);
		const HRESULT hr = native_present(may_wait ? 0 : DXGI_PRESENT_DO_NOT_WAIT);
		if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
		{
			--g.win.native_presents; --g.native_total;   // nothing was presented
			++g.win.wsd;
			g.r_in_bb = true;                            // stays pending; retried later
			return;
		}
		const bool retry = g.r_in_bb;
		g.pending = false; g.r_in_bb = false;
		if (retry) ++g.win.retries_ok;
		if (rec_q *r = open_rec())
		{
			if (kind == A_LATE) r->late = true;
			else if (r->r_kind == A_NONE) r->r_kind = retry ? g.r_kind_first : kind;
		}
		switch (kind)
		{
		case A_PRE: ++g.win.r_pre; break;
		case A_POST: ++g.win.r_post; break;
		case A_DEAD: ++g.win.r_dead; break;
		default: ++g.win.r_late; ++g.late_total; break;
		}
		handle_hr(hr, kind == A_LATE ? "late R present" : "R present");
	}

	int inject_kind()
	{
		return g_cfg.inject_at == 0 ? g.anchor_auto : g_cfg.inject_at;
	}

	void anchor(int kind)
	{
		ts(kind == A_PRE ? TS_PRE : (kind == A_POST ? TS_POST : TS_DEAD));
		const double t0 = qpc_ms();
		if (kind == A_PRE) push_cpu(g.win.c_to_pre, g.t_frame_start, t0);
		if (kind == A_POST && g.t_pre > 0.0) { push_cpu(g.win.c_dlss, g.t_pre, t0); g.t_pre = 0.0; }
		if (g.pending && (kind == A_DEAD || kind == inject_kind() || g.r_in_bb)) inject_r(kind);
		if (kind == A_PRE) g.t_pre = qpc_ms();   // after our own present: pre->post measures SR's DLSS+NR evaluate on the CPU
	}

	bool on_imm(command_list *cmd)
	{
		return cmd != nullptr && g.imm != nullptr && reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native()) == g.imm;
	}

	void check_post()
	{
		if (!g.f.after_tsaa) return;
		g.f.after_tsaa = false;
		if (!g.f.post) { g.f.post = true; anchor(A_POST); }
	}

	void check_dead_resource(uint64_t r)
	{
		if (g.f.dead || g.bb == 0 || r != g.bb) return;
		g.f.dead = true;
		anchor(A_DEAD);
	}

	// ------------------------------------------------------------------ PNG dump (stored deflate, no compression library)
	namespace png
	{
		uint32_t crc_tab[256];
		bool crc_init = false;
		uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
		{
			if (!crc_init)
			{
				for (uint32_t i = 0; i < 256; ++i) { uint32_t x = i; for (int k = 0; k < 8; ++k) x = (x & 1) ? 0xEDB88320u ^ (x >> 1) : x >> 1; crc_tab[i] = x; }
				crc_init = true;
			}
			c ^= 0xFFFFFFFFu;
			for (size_t i = 0; i < n; ++i) c = crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
			return c ^ 0xFFFFFFFFu;
		}
		void be32(std::vector<uint8_t> &o, uint32_t v) { o.push_back(uint8_t(v >> 24)); o.push_back(uint8_t(v >> 16)); o.push_back(uint8_t(v >> 8)); o.push_back(uint8_t(v)); }
		void chunk(std::vector<uint8_t> &o, const char *type, const uint8_t *data, size_t n)
		{
			be32(o, static_cast<uint32_t>(n));
			const size_t at = o.size();
			o.insert(o.end(), type, type + 4);
			if (n) o.insert(o.end(), data, data + n);
			be32(o, crc(0, o.data() + at, n + 4));
		}
		// rgba: w*h*4 bytes tightly packed (R,G,B,A); bgr=true when the source is BGRA
		bool write_rgb(const std::filesystem::path &path, const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h, bool bgr)
		{
			if (w == 0 || h == 0 || rgba.size() < static_cast<size_t>(w) * h * 4) return false;
			const size_t row = static_cast<size_t>(w) * 3 + 1;
			std::vector<uint8_t> raw(row * h);
			for (uint32_t y = 0; y < h; ++y)
			{
				uint8_t *d = raw.data() + row * y;
				const uint8_t *s = rgba.data() + static_cast<size_t>(w) * 4 * y;
				d[0] = 0;
				for (uint32_t x = 0; x < w; ++x)
				{
					d[1 + x * 3 + 0] = s[x * 4 + (bgr ? 2 : 0)];
					d[1 + x * 3 + 1] = s[x * 4 + 1];
					d[1 + x * 3 + 2] = s[x * 4 + (bgr ? 0 : 2)];
				}
			}
			std::vector<uint8_t> z;
			z.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
			z.push_back(0x78); z.push_back(0x01);
			uint32_t a = 1, b = 0;
			for (size_t i = 0; i < raw.size(); ++i) { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
			for (size_t off = 0; off < raw.size();)
			{
				const size_t n = std::min<size_t>(65535, raw.size() - off);
				const bool last = off + n == raw.size();
				z.push_back(last ? 1 : 0);
				z.push_back(uint8_t(n & 0xFF)); z.push_back(uint8_t(n >> 8));
				z.push_back(uint8_t(~n & 0xFF)); z.push_back(uint8_t((~n >> 8) & 0xFF));
				z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
				off += n;
			}
			be32(z, (b << 16) | a);
			std::vector<uint8_t> o;
			o.reserve(z.size() + 64);
			const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
			o.insert(o.end(), sig, sig + 8);
			uint8_t ihdr[13] = {};
			ihdr[0] = uint8_t(w >> 24); ihdr[1] = uint8_t(w >> 16); ihdr[2] = uint8_t(w >> 8); ihdr[3] = uint8_t(w);
			ihdr[4] = uint8_t(h >> 24); ihdr[5] = uint8_t(h >> 16); ihdr[6] = uint8_t(h >> 8); ihdr[7] = uint8_t(h);
			ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
			chunk(o, "IHDR", ihdr, 13);
			chunk(o, "IDAT", z.data(), z.size());
			chunk(o, "IEND", nullptr, 0);
			FILE *f = _wfsopen(path.c_str(), L"wb", _SH_DENYNO);
			if (!f) return false;
			const bool ok = fwrite(o.data(), 1, o.size(), f) == o.size();
			fclose(f);
			return ok;
		}
	}

	namespace dump
	{
		// images: 0 prev = real[k-1], 1 gen, 2 cur = real[k], 3 HUD-less(k) from the bridge (Mode 3 with FsrHudless=1 only)
		constexpr int kImg = 4;
		ID3D11Texture2D *stg[kImg] = {};
		std::vector<uint8_t> px[kImg];
		bool got[kImg] = {}, want[kImg] = {};
		int stage = 0;               // 0 idle, 1 copies queued, waiting for the GPU
		uint64_t queued_at = 0, f_prev = 0, f_cur = 0, next_allowed = 0;
		int count = 0;
		uint32_t w = 0, h = 0;
		bool bgr = false, key_down = false, key_armed = false;
		int mode = 0; bool marker = false;
		float changed_at_queue = -1.0f;

		void release() { for (auto *&t : stg) safe_release(t); for (auto &v : px) { v.clear(); v.shrink_to_fit(); } stage = 0; w = h = 0; }

		// DumpTriplet=N: the N-th generated frame, then every DumpEvery generated frames (DumpMax in total). With
		// DumpMinChanged=P only frames whose latest cut-guard read-back had P % <= changed samples <= CutPercent qualify, and
		// the next dump comes DumpEvery generated frames after the previous one.
		bool due(uint64_t g_count)
		{
			if (key_armed) return true;
			if (g_cfg.dump_triplet <= 0 || count >= g_cfg.dump_max) return false;
			const uint64_t first = static_cast<uint64_t>(g_cfg.dump_triplet);
			if (g_cfg.dump_min_changed > 0.0f)
			{
				if (g_count < first || g_count < next_allowed) return false;
				if (g.last_cut_frac < 0.0f || g.last_cut_frac_frame + 8 < g.frame_rec) return false;   // no fresh read-back
				const float pc = g.last_cut_frac * 100.0f;
				return pc >= g_cfg.dump_min_changed && pc <= g_cfg.cut_percent;
			}
			if (g_count == first) return true;
			if (g_cfg.dump_every > 0 && g_count > first && (g_count - first) % static_cast<uint64_t>(g_cfg.dump_every) == 0) return true;
			return false;
		}

		// right after G was built in reshade_present(k), before any marker: prev = real[(k-1)&1], gen, cur = real[k&1], hud
		void queue(int slot_prev, int slot_cur, uint64_t k, int m, ID3D11Texture2D *hud, uint64_t g_count)
		{
			if (stage != 0 || !g.dev || !g.imm) return;
			if (w != g.w || h != g.h) { release(); }
			D3D11_TEXTURE2D_DESC d = {};
			d.Width = g.w; d.Height = g.h; d.MipLevels = 1; d.ArraySize = 1; d.Format = g.fmt_unorm; d.SampleDesc.Count = 1;
			d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			bool hud_ok = false;
			if (hud)
			{
				D3D11_TEXTURE2D_DESC hd = {}; hud->GetDesc(&hd);
				DXGI_FORMAT tl = DXGI_FORMAT_UNKNOWN, sr = DXGI_FORMAT_UNKNOWN, un = DXGI_FORMAT_UNKNOWN;
				hud_ok = hd.Width == g.w && hd.Height == g.h && hd.SampleDesc.Count == 1 && hd.ArraySize == 1 && hd.MipLevels == 1 &&
				         format_family(hd.Format, tl, sr, un) && tl == g.fmt_tl;
			}
			for (int i = 0; i < kImg; ++i) want[i] = i < 3 || hud_ok;
			for (int i = 0; i < kImg; ++i)
				if (want[i] && !stg[i] && (FAILED(g.dev->CreateTexture2D(&d, nullptr, &stg[i])) || !stg[i]))
				{
					stg[i] = nullptr; logf("dump: staging texture creation FAILED"); release(); key_armed = false; ++count; return;
				}
			w = g.w; h = g.h; bgr = g.fmt_unorm == DXGI_FORMAT_B8G8R8A8_UNORM;
			{
				pred_guard pg(g.imm);
				g.imm->CopyResource(stg[0], g.real[slot_prev]);
				g.imm->CopyResource(stg[1], g.out.tex ? g.out.tex : g.gen);
				g.imm->CopyResource(stg[2], g.real[slot_cur]);
				if (want[3]) g.imm->CopyResource(stg[3], hud);
			}
			for (bool &b : got) b = false;
			stage = 1; queued_at = g.frame_rec; f_prev = k - 1; f_cur = k; mode = m; marker = g_cfg.marker != 0;
			changed_at_queue = g.last_cut_frac;
			next_allowed = g_count + static_cast<uint64_t>(g_cfg.dump_every > 0 ? g_cfg.dump_every : 1);
			key_armed = false;
			++count;
			logf("dump %d: queued read-back of real[%llu], gen, real[%llu]%s (mode %d, latest cut-guard changed %.1f %%, FSR debug inputs phase %c flags %d)", count, f_prev, f_cur,
				want[3] ? ", hudless" : "", m, changed_at_queue * 100.0f, g.dbg_phase, g.dbg_flags);
		}

		// from the present event: map without waiting, write the PNGs once all images are back
		void poll()
		{
			if (stage != 1 || !g.imm) return;
			if (g.frame_rec < queued_at + 2) return;
			for (int i = 0; i < kImg; ++i)
			{
				if (!want[i] || got[i] || !stg[i]) continue;
				D3D11_MAPPED_SUBRESOURCE ms = {};
				const HRESULT hr = g.imm->Map(stg[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms);
				if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
				if (FAILED(hr) || ms.pData == nullptr || ms.RowPitch < w * 4)
				{
					if (SUCCEEDED(hr)) g.imm->Unmap(stg[i], 0);
					logf("dump: Map FAILED 0x%08x", static_cast<unsigned>(hr)); stage = 0; return;
				}
				px[i].resize(static_cast<size_t>(w) * h * 4);
				for (uint32_t y = 0; y < h; ++y)
					memcpy(px[i].data() + static_cast<size_t>(w) * 4 * y, static_cast<const uint8_t *>(ms.pData) + static_cast<size_t>(ms.RowPitch) * y, static_cast<size_t>(w) * 4);
				g.imm->Unmap(stg[i], 0);
				got[i] = true;
			}
			for (int i = 0; i < kImg; ++i) if (want[i] && !got[i]) return;
			stage = 0;
			const char *names[kImg] = { "a_prev", "b_gen", "c_cur", "d_hudless" };
			bool ok = true;
			for (int i = 0; i < kImg; ++i)
			{
				if (!want[i]) continue;
				char n[96]; snprintf(n, sizeof(n), "tf2-fg-dump%d-%s.png", count, names[i]);
				ok = png::write_rgb(g_dir / n, px[i], w, h, bgr) && ok;
			}
			// quick numeric check next to the images (8-bit codes, every 2nd pixel, outside the top-left 192x48 marker area)
			double lut[256];
			for (int i = 0; i < 256; ++i) { const double v = i / 255.0; lut[i] = v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
			auto enc = [](double v) { v = v < 0 ? 0 : (v > 1 ? 1 : v); return (v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055) * 255.0; };
			double diff = 0.0, motion = 0.0; uint64_t n = 0;
			double mv_blend = 0.0, mv_prev = 0.0, mv_cur = 0.0; uint64_t n_mv = 0;
			uint64_t hud_px = 0, hud_eq = 0, nonhud_eq_cur = 0, nonhud = 0; double hud_d = 0.0;
			for (uint32_t y = 0; y < h; y += 2)
				for (uint32_t x = 0; x < w; x += 2)
				{
					if (x < 192 && y < 48) continue;
					const size_t o = (static_cast<size_t>(y) * w + x) * 4;
					int dmax = 0; double db = 0.0, dp = 0.0, dc = 0.0; bool is_hud = false, eq_cur = true;
					for (int c = 0; c < 3; ++c)
					{
						const uint8_t a = px[0][o + c], b = px[2][o + c], gv = px[1][o + c];
						const double blend = enc(lut[a] + (lut[b] - lut[a]) * g_cfg.blend_t);
						const double expect = mode == 1 ? static_cast<double>(a) : blend;
						diff += std::fabs(gv - expect);
						motion += std::abs(static_cast<int>(a) - static_cast<int>(b));
						++n;
						dmax = std::max(dmax, std::abs(static_cast<int>(a) - static_cast<int>(b)));
						db += std::fabs(gv - blend); dp += std::abs(static_cast<int>(gv) - static_cast<int>(a)); dc += std::abs(static_cast<int>(gv) - static_cast<int>(b));
						if (want[3] && px[3][o + c] != b) is_hud = true;
						if (gv != b) eq_cur = false;
					}
					if (dmax > 8) { mv_blend += db / 3.0; mv_prev += dp / 3.0; mv_cur += dc / 3.0; ++n_mv; }
					if (want[3])
					{
						if (is_hud)
						{
							++hud_px; if (eq_cur) ++hud_eq;
							for (int c = 0; c < 3; ++c) hud_d += std::abs(static_cast<int>(px[1][o + c]) - static_cast<int>(px[2][o + c])) / 3.0;
						}
						else { ++nonhud; if (eq_cur) ++nonhud_eq_cur; }
					}
				}
			const double nd = n ? static_cast<double>(n) : 1.0, nm = n_mv ? static_cast<double>(n_mv) : 1.0, nh = hud_px ? static_cast<double>(hud_px) : 1.0;
			const double samples = static_cast<double>(((w + 1) / 2) * ((h + 1) / 2));
			logf("dump %d: wrote %s tf2-fg-dump%d-{a_prev,b_gen,c_cur%s}.png (%ux%u, frames %llu/%llu, mode %d, marker %d) | mean |prev-cur| %.2f codes, mean |gen-lerp| %.2f codes | "
				"moving px (|prev-cur| > 8) %.1f %%: |gen-lerp| %.2f |gen-prev| %.2f |gen-cur| %.2f",
				count, ok ? "" : "(WRITE FAILED)", count, want[3] ? ",d_hudless" : "", w, h, f_prev, f_cur, mode, marker ? 1 : 0, motion / nd, diff / nd,
				samples > 0.0 ? 100.0 * static_cast<double>(n_mv) / samples : 0.0, mv_blend / nm, mv_prev / nm, mv_cur / nm);
			if (want[3])
				logf("dump %d: HUD (cur != hudless) %.2f %% of px: gen == cur on %.2f %% of them, mean |gen-cur| %.3f codes | non-HUD px with gen == cur %.1f %%",
					count, samples > 0.0 ? 100.0 * static_cast<double>(hud_px) / samples : 0.0, 100.0 * static_cast<double>(hud_eq) / nh, hud_d / nh,
					nonhud ? 100.0 * static_cast<double>(nonhud_eq_cur) / static_cast<double>(nonhud) : 0.0);
			for (auto &v : px) { v.clear(); v.shrink_to_fit(); }
		}
	}

	// ------------------------------------------------------------------ G
	// Our compute work (M1b blend, the GPU cut guard, the Mode 3 select pass) with our own bindings: t0 = prev, t1 = cur
	// (_SRGB SRVs), u0 = gen (UNORM UAV), u1 = the cut-guard counter, b0 = Params. diff = clear the counter + diff pass;
	// stat = bind the counter (the select/blend pass reads it); main_cs = blend_main or select_main (or none). D3D11 orders
	// the dispatches on the shared UAV. After a diff pass the counter is copied into the read-back ring (log + DumpMinChanged).
	void cs_work(ID3D11ShaderResourceView *const srvs[2], bool diff, bool stat, ID3D11ComputeShader *main_cs)
	{
		const UINT gx = (g.w + 7) / 8, gy = (g.h + 7) / 8;
		const UINT sx = (g.w + 7) / 8, sy = (g.h + 7) / 8, dx = (sx + 7) / 8, dy = (sy + 7) / 8;
		ID3D11ShaderResourceView *null_srv[2] = {};
		ID3D11UnorderedAccessView *null_uav[2] = {};
		ID3D11UnorderedAccessView *uavs[2] = { g.out.uav, (diff || stat) ? g.stat_uav : nullptr };
		g.imm->CSSetConstantBuffers(0, 1, &g.cb);
		g.imm->CSSetShaderResources(0, 2, srvs);
		if (diff)
		{
			const UINT zero[4] = {};
			g.imm1->ClearUnorderedAccessViewUint(g.stat_uav, zero);
		}
		g.imm->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
		if (diff)
		{
			g.imm->CSSetShader(g.diff_cs, nullptr, 0);
			g.imm->Dispatch(dx, dy, 1);
		}
		if (main_cs)
		{
			g.imm->CSSetShader(main_cs, nullptr, 0);
			g.imm->Dispatch(gx, gy, 1);
		}
		g.imm->CSSetUnorderedAccessViews(0, 2, null_uav, nullptr);   // no references to our resources stay bound
		g.imm->CSSetShaderResources(0, 2, null_srv);
		if (diff)
		{
			// 0.3.1: tagged with the game frame k whose R(k-1) -> R(k) pair this is (called from reshade_present(k), where
			// frame_rec is already k + 1; 0.3.0 logged k + 1)
			const int slot = static_cast<int>(g.frame_rec & 3);
			if (g.stat_stg[slot]) { g.imm->CopyResource(g.stat_stg[slot], g.stat); g.stat_frame[slot] = g.present_k; }
		}
	}

	// Runs cs_work in our private context state (SwapDeviceContextState at the device's own FL), or with a manual save/restore
	// of exactly the CS slots we touch (StateMode=1). false = nothing was run.
	bool run_cs(ID3D11ShaderResourceView *const srvs[2], bool diff, bool stat, ID3D11ComputeShader *main_cs)
	{
		if (g_cfg.state_mode == 0 && g.state && g.imm1)
		{
			ID3DDeviceContextState *app = nullptr;
			g.imm1->SwapDeviceContextState(g.state, &app);
			if (app == nullptr)
			{
				// SwapDeviceContextState is void. On success it always hands back the previously active state object (the
				// immediate context always has one), so a null result means the call was rejected and NO swap happened:
				// the game's state is still bound and untouched. Do not touch the context; use the manual path from now on.
				logf("blend: SwapDeviceContextState returned no previous state (call rejected, nothing swapped) - switching to manual CS save/restore");
				g_cfg.state_mode = 1;
				return false;
			}
			cs_work(srvs, diff, stat, main_cs);
			g.imm1->SwapDeviceContextState(app, nullptr);
			app->Release();
			return true;
		}
		// manual: save exactly the CS slots we touch (shader + class instances, t0-t1, u0-u1, b0), restore them afterwards
		ID3D11ComputeShader *o_cs = nullptr; ID3D11ClassInstance *o_ci[256] = {}; UINT o_nci = 256;
		g.imm->CSGetShader(&o_cs, o_ci, &o_nci);
		if (o_nci > 256) o_nci = 256;
		ID3D11ShaderResourceView *o_srv[2] = {}; ID3D11UnorderedAccessView *o_uav[2] = {}; ID3D11Buffer *o_cb[1] = {};
		g.imm->CSGetShaderResources(0, 2, o_srv); g.imm->CSGetUnorderedAccessViews(0, 2, o_uav); g.imm->CSGetConstantBuffers(0, 1, o_cb);
		cs_work(srvs, diff, stat, main_cs);
		g.imm->CSSetShader(o_cs, o_nci ? o_ci : nullptr, o_nci);
		g.imm->CSSetShaderResources(0, 2, o_srv); g.imm->CSSetUnorderedAccessViews(0, 2, o_uav, nullptr); g.imm->CSSetConstantBuffers(0, 1, o_cb);
		if (o_cs) o_cs->Release();
		for (UINT i = 0; i < o_nci; ++i) if (o_ci[i]) o_ci[i]->Release();
		for (auto *v : o_srv) if (v) v->Release();
		for (auto *v : o_uav) if (v) v->Release();
		for (auto *v : o_cb) if (v) v->Release();
		return true;
	}

	// Log + DumpMinChanged: read back the cut-guard counters of earlier frames without waiting.
	void poll_cut_stats()
	{
		if (!g.cut_guard_ok || !g.imm) return;
		const float total = static_cast<float>(((g.w + 7) / 8) * ((g.h + 7) / 8));
		for (int i = 0; i < 4; ++i)
		{
			if (g.stat_frame[i] == ~0ull || !g.stat_stg[i] || g.stat_frame[i] + 3 > g.frame_rec) continue;   // >= 2 frames old (frame tag = k)
			D3D11_MAPPED_SUBRESOURCE ms = {};
			const HRESULT hr = g.imm->Map(g.stat_stg[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms);
			if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
			const uint64_t f = g.stat_frame[i];
			g.stat_frame[i] = ~0ull;
			if (FAILED(hr) || !ms.pData) continue;
			uint32_t changed = 0; memcpy(&changed, ms.pData, 4);
			g.imm->Unmap(g.stat_stg[i], 0);
			if (total <= 0.0f) continue;
			const float frac = static_cast<float>(changed) / total;
			++g.win.cut_samples; g.win.cut_frac_sum += frac;
			if (frac > g.win.cut_frac_max) g.win.cut_frac_max = frac;
			if (f >= g.last_cut_frac_frame) { g.last_cut_frac = frac; g.last_cut_frac_frame = f; }
			if (frac * 100.0f > g_cfg.cut_percent)
			{
				++g.win.cut_gpu;
				if (g.cut_logged++ < 40) logf("cut guard: frame %llu (R(k-1) -> R(k), read back at frame %llu) - %.1f %% of sampled pixels changed -> gen = current frame (no interpolation)", f, g.frame_rec, frac * 100.0f);
			}
		}
	}

	bool cut_guard_usable()
	{
		return g.cut_guard_ok && g.imm1 && g.diff_cs && g.stat_uav && g_cfg.cut_percent > 0.0f;
	}

	// Params for all our passes (written once per frame, DISCARD).
	bool write_params(bool guard)
	{
		const UINT sx = (g.w + 7) / 8, sy = (g.h + 7) / 8;          // samples of the diff pass (every 8th pixel)
		struct { uint32_t w, h; float t, cut_frac, cut_delta, total, pad0, pad1; } p =
			{ g.w, g.h, g_cfg.blend_t, guard ? g_cfg.cut_percent * 0.01f : 0.0f, g_cfg.cut_delta, static_cast<float>(sx * sy), 0.0f, 0.0f };
		static_assert(sizeof(p) == 32, "cbuffer Params is 32 bytes");
		D3D11_MAPPED_SUBRESOURCE ms = {};
		if (!g.cb || FAILED(g.imm->Map(g.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)) || !ms.pData) return false;
		memcpy(ms.pData, &p, sizeof(p));   // the constant buffer is 32 bytes (0.2.0 created it with 16: see PROGRESS_M3.log)
		g.imm->Unmap(g.cb, 0);
		return true;
	}

	// M1b: gen = lerp(real[prev], real[cur], t) in linear light (gen = cur when the cut guard fires).
	bool build_blend(int slot_prev, int slot_cur)
	{
		if (!ensure_blend() || !g.real_srv[slot_prev] || !g.real_srv[slot_cur] || !g.out.uav) return false;
		const bool guard = cut_guard_usable();
		if (!write_params(guard)) return false;
		ID3D11ShaderResourceView *srvs[2] = { g.real_srv[slot_prev], g.real_srv[slot_cur] };
		return run_cs(srvs, guard, guard, g.cs);
	}

	// ------------------------------------------------------------------ Marker=2: fgmark.h barcode (dupmon display delivery)
	// 16 cells of 8x8 at (64,0): cell 0 white, cell 1 black, cells 2-3 type (1 R, 2 G), cells 4-15 seq = (2*frame + isR) & 4095.
	ID3D11RenderTargetView *bb_rtv()
	{
		if (g.bb_rtv) return g.bb_rtv;
		static uint64_t failed_gen = 0;
		if (!g.dev || !g.bb || g.fmt_srgb == DXGI_FORMAT_UNKNOWN || failed_gen == g.sc_gen) return nullptr;
		D3D11_RENDER_TARGET_VIEW_DESC rd = {};
		rd.Format = g.fmt_srgb; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		if (FAILED(g.dev->CreateRenderTargetView(reinterpret_cast<ID3D11Resource *>(g.bb), &rd, &g.bb_rtv)) || !g.bb_rtv)
		{
			g.bb_rtv = nullptr; failed_gen = g.sc_gen;
			logf("marker: back-buffer RTV creation FAILED - no barcode on R frames");
		}
		return g.bb_rtv;
	}

	void stamp_barcode(ID3D11RenderTargetView *rtv, bool is_r, uint64_t frame)
	{
		if (!(g_cfg.marker & 2) || !rtv || !g.imm1) return;
		const uint32_t seq = static_cast<uint32_t>((2 * frame + (is_r ? 1 : 0)) & 4095);
		const int type = is_r ? 1 : 2;
		D3D11_RECT white[16], black[16]; UINT nw = 0, nb = 0;
		for (int i = 0; i < 16; ++i)
		{
			bool bit = false;
			if (i == 0) bit = true;
			else if (i == 1) bit = false;
			else if (i < 4) bit = ((type >> (i - 2)) & 1) != 0;
			else bit = ((seq >> (i - 4)) & 1) != 0;
			const D3D11_RECT r = { 64 + 8 * i, 0, 72 + 8 * i, 8 };
			if (bit) white[nw++] = r; else black[nb++] = r;
		}
		const float w1[4] = { 1.0f, 1.0f, 1.0f, 1.0f }, b0[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		pred_guard pg(g.imm);
		if (nw) g.imm1->ClearView(rtv, w1, white, nw);
		if (nb) g.imm1->ClearView(rtv, b0, black, nb);
	}

	// ------------------------------------------------------------------ Mode 3: FSR 3 frame interpolation
	// Exports of tf2-dlss-sr / tf2-nr-bridge (tf2_fg_interop.h v1). The module handle is checked on every call: a reloaded
	// add-on gets its function re-resolved, an unloaded one is never called.
	bool resolve_sr()
	{
		const HMODULE m = GetModuleHandleW(L"tf2-dlss-sr.addon64");
		if (m != fsr::sr_mod)
		{
			fsr::sr_mod = m;
			fsr::get_sr = m ? reinterpret_cast<PFN_tf2sr_get_fg_inputs>(GetProcAddress(m, "tf2sr_get_fg_inputs")) : nullptr;
			fsr::sr_gen = ~0ull; fsr::need_reset = true;
			if (!fsr::sr_logged || !fsr::get_sr) logf("fsr: tf2-dlss-sr.addon64 %s, export tf2sr_get_fg_inputs %s", m ? "loaded" : "NOT loaded", fsr::get_sr ? "resolved" : "MISSING");
			fsr::sr_logged = true;
		}
		return fsr::get_sr != nullptr;
	}
	bool resolve_hud()
	{
		const HMODULE m = GetModuleHandleW(L"tf2-nr-bridge.addon64");
		if (m != fsr::hud_mod)
		{
			fsr::hud_mod = m;
			fsr::get_hud = m ? reinterpret_cast<PFN_tf2nr_get_hudless>(GetProcAddress(m, "tf2nr_get_hudless")) : nullptr;
			fsr::hud_gen = ~0ull; fsr::need_reset = true;
			if (!fsr::hud_logged || !fsr::get_hud) logf("fsr: tf2-nr-bridge.addon64 %s, export tf2nr_get_hudless %s", m ? "loaded" : "NOT loaded", fsr::get_hud ? "resolved" : "MISSING");
			fsr::hud_logged = true;
		}
		return fsr::get_hud != nullptr;
	}

	// AddRef'd export results, released at scope end.
	struct sr_in
	{
		tf2sr_fg_inputs_v1 io = {};
		int ret = 0;
		bool valid = false;
		~sr_in() { safe_release(io.depth); safe_release(io.motion_vectors); safe_release(io.exposure); }
	};
	struct hud_in
	{
		tf2nr_hudless_v1 io = {};
		int ret = 0;
		bool valid = false;
		~hud_in() { safe_release(io.texture); }
	};

	void query_sr(sr_in &s)
	{
		s.io = {};
		s.io.struct_size = sizeof(s.io);
		if (!resolve_sr()) return;
		try { s.ret = fsr::get_sr(&s.io); } catch (...) { s.ret = -2; }
		++g.win.sr_polls;
		if (s.ret != 1) { s.io.depth = s.io.motion_vectors = s.io.exposure = nullptr; return; }   // nothing AddRef'd unless VALID
		s.valid = (s.io.flags & TF2SR_FG_VALID) && s.io.depth && s.io.motion_vectors && s.io.render_width >= 64 && s.io.render_height >= 64 &&
		          s.io.render_width <= g.w && s.io.render_height <= g.h && s.io.display_width == g.w && s.io.display_height == g.h;
		if (s.valid)
		{
			D3D11_TEXTURE2D_DESC dd = {}, md = {};
			s.io.depth->GetDesc(&dd); s.io.motion_vectors->GetDesc(&md);
			s.valid = dd.Width == s.io.render_width && dd.Height == s.io.render_height && md.Width == s.io.render_width && md.Height == s.io.render_height;
		}
		// 0.3.1: the FSR library is built for jitter-free MVs and standard (non-reversed) depth; an SR config/runtime change
		// ([TF2DLSSSR] MVJittered=0 / MVKey, DepthInverted=1) must not feed it the other convention silently
		if (s.valid && ((s.io.flags & TF2SR_FG_MV_JITTER_FREE) == 0 || (s.io.flags & TF2SR_FG_DEPTH_INVERTED) != 0))
		{
			s.valid = false; ++g.win.sr_conv;
			if (!fsr::conv_logged)
			{
				fsr::conv_logged = true;
				logf("fsr: the SR export's conventions do not match the FSR library (flags 0x%x: MV_JITTER_FREE %s, DEPTH_INVERTED %s; FSR needs jitter-free motion vectors and standard depth) - no FSR on such frames",
					s.io.flags, (s.io.flags & TF2SR_FG_MV_JITTER_FREE) ? "yes" : "NO", (s.io.flags & TF2SR_FG_DEPTH_INVERTED) ? "YES" : "no");
			}
		}
		if (s.valid) ++g.win.sr_valid;
	}

	void query_hud(hud_in &hdl)
	{
		hdl.io = {};
		hdl.io.struct_size = sizeof(hdl.io);
		if (!g_cfg.fsr_hudless || !resolve_hud()) return;
		try { hdl.ret = fsr::get_hud(&hdl.io); } catch (...) { hdl.ret = -2; }
		++g.win.hud_polls;
		if (hdl.ret != 1) { hdl.io.texture = nullptr; return; }
		hdl.valid = (hdl.io.flags & TF2NR_HUDLESS_VALID) && hdl.io.texture && hdl.io.width == g.w && hdl.io.height == g.h;
		if (hdl.valid)
		{
			D3D11_TEXTURE2D_DESC d = {}; hdl.io.texture->GetDesc(&d);
			DXGI_FORMAT tl = DXGI_FORMAT_UNKNOWN, sr = DXGI_FORMAT_UNKNOWN, un = DXGI_FORMAT_UNKNOWN;
			hdl.valid = d.Width == g.w && d.Height == g.h && d.SampleDesc.Count == 1 && d.ArraySize == 1 && format_family(d.Format, tl, sr, un) && tl == g.fmt_tl;
		}
		if (hdl.valid) ++g.win.hud_valid;
	}

	// tf2fg::destroy records on the immediate context. It runs on the render thread (fsr_check, update_state) or at device
	// teardown / device replacement (destroy_device, init_swapchain of a new device), when the game itself cannot be using
	// that context (review M3: no change needed; an off-thread call is logged).
	// 0.4.0 debug FSR inputs (review finding F7: prove in game that FSR consumes the game's motion vectors and depth).
	// Bitmask: 1 = zero motion vectors (RG16F zeros), 2 = flat depth (R32F 0.99 everywhere: no depth edges), 4 = optical flow
	// off (FI interpolates from the MVs + depth alone). FsrDebugA applies, or with FsrDebugAB = N the flags alternate A / B
	// every N FSR frames; every switch resets FSR (no mixed history).
	namespace fdbg
	{
		ID3D11Texture2D *zero_mv = nullptr, *flat_depth = nullptr;
		uint32_t w = 0, h = 0;
		bool failed = false, phase_b = false;
		int applied = -1;            // flags applied to the current context (-1 = none yet: re-applied after every create)
		uint64_t phase_count = 0, switches = 0;
		void release() { safe_release(zero_mv); safe_release(flat_depth); w = h = 0; }
		bool ensure(uint32_t rw, uint32_t rh)
		{
			if (zero_mv && flat_depth && w == rw && h == rh) return true;
			if (failed || !g.dev || rw == 0 || rh == 0 || rw > 16384 || rh > 16384) return false;
			release();
			std::vector<uint32_t> z(static_cast<size_t>(rw) * rh, 0u);
			std::vector<float> d(static_cast<size_t>(rw) * rh, 0.99f);
			D3D11_TEXTURE2D_DESC td = {};
			td.Width = rw; td.Height = rh; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			td.Format = DXGI_FORMAT_R16G16_FLOAT;
			D3D11_SUBRESOURCE_DATA sd = { z.data(), rw * 4, 0 };
			bool ok = SUCCEEDED(g.dev->CreateTexture2D(&td, &sd, &zero_mv)) && zero_mv;
			td.Format = DXGI_FORMAT_R32_FLOAT; sd.pSysMem = d.data();
			ok = ok && SUCCEEDED(g.dev->CreateTexture2D(&td, &sd, &flat_depth)) && flat_depth;
			if (!ok) { release(); failed = true; logf("fsr debug: input texture creation FAILED - debug inputs off"); return false; }
			w = rw; h = rh;
			return true;
		}
		// flags for this FSR frame; sets `reset` when they change (and re-applies the optical-flow switch)
		int frame_flags(bool &reset)
		{
			int want = g_cfg.fsr_dbg_a;
			if (g_cfg.fsr_dbg_ab > 0)
			{
				if (phase_count >= static_cast<uint64_t>(g_cfg.fsr_dbg_ab)) { phase_b = !phase_b; phase_count = 0; ++switches; }
				want = phase_b ? g_cfg.fsr_dbg_b : g_cfg.fsr_dbg_a;
				++phase_count;
			}
			g.dbg_phase = g_cfg.fsr_dbg_ab > 0 ? (phase_b ? 'B' : 'A') : '-';
			if (want != applied && fsr::ctx)
			{
				tf2fg::setOpticalFlowEnabled(fsr::ctx, (want & 4) == 0);
				if (switches < 200 || applied < 0)
					logf("fsr debug inputs: phase %c flags %d (%s%s%s%s) - FSR reset", g.dbg_phase, want, want == 0 ? "normal" : "",
						(want & 1) ? "zero MVs " : "", (want & 2) ? "flat depth " : "", (want & 4) ? "optical flow OFF" : "");
				applied = want;
				reset = true;
			}
			g.dbg_flags = want;
			return want;
		}
	}

	void fsr_destroy(const char *why)
	{
		if (!fsr::ctx) return;
		const DWORD tid = GetCurrentThreadId();
		tf2fg::destroy(fsr::ctx);
		fsr::ctx = nullptr;
		fsr::need_reset = true;
		fsr::last_disp = ~0ull; fsr::last_had_hud = false;
		if (why) logf("fsr: context destroyed (%s)%s", why, (g.render_tid != 0 && tid != g.render_tid) ? " - not on the render thread (device teardown)" : "");
	}

	// The FL gate: FSR FI needs FL 11_1. Called with the device known.
	bool fsr_fl_ok()
	{
		if (!g.dev) return false;
		if (g.fl >= D3D_FEATURE_LEVEL_11_1) return true;
		if (!fsr::unavailable)
		{
			fsr::unavailable = true;
			fsr::why = "the game's D3D11 device is feature level 11_0 - RESTART REQUIRED";
		}
		return false;
	}

	// Drops a context that no longer fits (display size) or failed. true = a usable context exists now. Never creates one
	// (0.3.1: creation happens only outside ACTIVE, in fsr_ensure; a 19-352 ms create inside FG was a hitch + CPU cut).
	bool fsr_check()
	{
		if (!fsr::ctx) return false;
		if (fsr::dw != g.w || fsr::dh != g.h) { fsr_destroy("display size changed"); return false; }
		if (tf2fg::isFailed(fsr::ctx))
		{
			fsr_destroy("library failure");
			++fsr::failures;
			fsr::retry_frame = g.frame_rec + 600;
			if (fsr::failures > 3)
			{
				fsr::unavailable = true; fsr::why = "the FSR library failed more than 3 times this session";
				logf("fsr: %s - frame generation OFF until the game restarts", fsr::why);
			}
			else logf("fsr: library failure %d - FG suspends; the context is re-created in 600 frames", fsr::failures);
			return false;
		}
		return true;
	}

	// Creates the context when needed (display size = back buffer, max render = display). false = no context this frame.
	// Called in WARMUP / SUSPENDED only (0.3.1).
	bool fsr_ensure()
	{
		if (fsr::unavailable || !g.dev || !g.imm || g.w == 0 || g.h == 0) return false;
		if (!fsr_fl_ok()) return false;
		if (g.fmt_tl != DXGI_FORMAT_R8G8B8A8_TYPELESS)
		{
			// 0.3.1: the library takes RGBA8 only (BGRA8 back buffers would run prepare() every frame and be refused at dispatch)
			fsr::unavailable = true;
			fsr::why = "the back buffer is not RGBA8 (FSR frame interpolation takes R8G8B8A8 only)";
			logf("fsr: back buffer typeless format %u - %s - frame generation OFF", static_cast<unsigned>(g.fmt_tl), fsr::why);
			return false;
		}
		if (fsr::ctx && (fsr::dw != g.w || fsr::dh != g.h)) fsr_destroy("display size changed");
		if (fsr::ctx && tf2fg::isFailed(fsr::ctx))
		{
			fsr_destroy("library failure");
			++fsr::failures;
			fsr::retry_frame = g.frame_rec + 600;
			if (fsr::failures > 3)
			{
				fsr::unavailable = true; fsr::why = "the FSR library failed more than 3 times this session";
				logf("fsr: %s - frame generation OFF until the game restarts", fsr::why);
				return false;
			}
			logf("fsr: library failure %d - re-creating the context in 600 frames", fsr::failures);
		}
		if (fsr::ctx) return true;
		if (g.frame_rec < fsr::retry_frame) return false;
		tf2fg::CreateOptions o;
		o.log = fsr::lib_log;
		o.logUser = nullptr;
		o.stateMode = g_cfg.state_mode == 1 ? tf2fg::StateMode::ManualComputeSave : tf2fg::StateMode::SwapContextState;
		o.gpuTiming = g_cfg.fsr_timing != 0;
		o.viewSpaceToMeters = 0.0254f;   // Source units (inches)
		o.minStackHeadroomKB = static_cast<uint32_t>(g_cfg.fsr_min_stack_kb);
		o.debugView = g_cfg.fsr_debug_view != 0;
		const double t0 = qpc_ms();
		tf2fg::Context *c = nullptr;
		const tf2fg::Status s = tf2fg::create(g.dev, g.w, g.h, g.w, g.h, &o, &c);   // g.dev = device::get_native() (never the proxy)
		++fsr::creates;
		if (s != tf2fg::Status::Ok || !c)
		{
			++fsr::failures;
			if (s == tf2fg::Status::Unsupported || s == tf2fg::Status::InvalidArgument || fsr::failures > 3)
			{
				fsr::unavailable = true;
				fsr::why = s == tf2fg::Status::Unsupported ? "the device lacks a capability FSR frame interpolation needs" : "the FSR context cannot be created";
				logf("fsr: create FAILED (%s) - %s - frame generation OFF for this session", tf2fg::statusName(s), fsr::why);
			}
			else
			{
				fsr::retry_frame = g.frame_rec + 600;
				logf("fsr: create FAILED (%s) - retry in 600 frames", tf2fg::statusName(s));
			}
			return false;
		}
		fsr::ctx = c; fsr::dw = g.w; fsr::dh = g.h; fsr::need_reset = true; fsr::last_disp = ~0ull; fsr::last_had_hud = false;
		fdbg::applied = -1;   // 0.4.0: the debug flags (optical-flow switch) are applied to the new context
		logf("fsr: context created in %.1f ms on the native device %p (FL 0x%x), display %ux%u, max render %ux%u, timing %d, state %s",
			qpc_ms() - t0, g.dev, static_cast<unsigned>(g.fl), g.w, g.h, g.w, g.h, g_cfg.fsr_timing, g_cfg.state_mode == 1 ? "manual CS save" : "SwapDeviceContextState");
		return true;
	}

	// One FSR frame at reshade_present(k). slot_cur holds R(k) (clean copy, HUD included). pair = real[k-1] is valid too.
	// Returns true when gen holds a presentable interpolated frame G(k-1/2).
	bool fsr_frame(uint64_t k, int slot_prev, int slot_cur, bool pair, const char *cut, sr_in &s, hud_in &hdl, int &dump_mode_out)
	{
		dump_mode_out = 3;
		if (!fsr_check()) { ++g.win.fsr_noctx; fsr::need_reset = true; return false; }   // (re)created outside ACTIVE only
		if (!s.valid)
		{
			++g.win.fsr_nosr; fsr::need_reset = true;
			return false;   // passthrough: FG needs DLSS SR's depth + motion vectors of this frame
		}
		// camera (b2 decode by tf2-dlss-sr); hold the last one briefly
		float cn = s.io.camera_near, cf = s.io.camera_far, cv = s.io.camera_fov_y;
		const bool cam = (s.io.flags & TF2SR_FG_CAMERA_VALID) && std::isfinite(cn) && std::isfinite(cf) && std::isfinite(cv) && cn > 0.0f && cf > cn && cv > 0.0f && cv < 3.1f;
		if (cam) { fsr::cam_near = cn; fsr::cam_far = cf; fsr::cam_fov = cv; fsr::cam_frame = k; fsr::cam_ok = true; }
		else if (fsr::cam_ok && k <= fsr::cam_frame + static_cast<uint64_t>(g_cfg.fsr_cam_hold)) { cn = fsr::cam_near; cf = fsr::cam_far; cv = fsr::cam_fov; ++g.win.fsr_camfb; }
		else
		{
			++g.win.fsr_nocam; fsr::need_reset = true;
			if (fsr::cam_logged++ < 10) logf("fsr: frame %llu has no decoded camera (SR flags 0x%x) and none within %d frames - no G", k, s.io.flags, g_cfg.fsr_cam_hold);
			return false;
		}
		if (!hdl.valid) ++g.win.fsr_nohud;
		// 0.3.1: tf2-dlss-sr's camera-cut hint (main view jumped: death cam, kill replay, respawn, spectator switch)
		const bool cam_cut = (s.io.flags & TF2SR_FG_CAMERA_CUT) != 0;
		// 0.3.1: the library's own discontinuity rule, so its reset dispatches are known here (no G for them)
		const bool disc = fsr::last_disp == ~0ull || k != fsr::last_disp + 1 || hdl.valid != fsr::last_had_hud;
		bool reset = fsr::need_reset || cut != nullptr || (s.io.flags & TF2SR_FG_RESET) != 0 || s.io.generation != fsr::sr_gen ||
		             (hdl.valid && hdl.io.generation != fsr::hud_gen) || cam_cut || disc;
		if (cam_cut)
		{
			++g.win.fsr_camcut;
			if (fsr::camcut_logged++ < 40) logf("camera cut: frame %llu (tf2-dlss-sr camera-cut hint) - FSR history reset, no generated frame", k);
		}
		if (disc && !fsr::need_reset && cut == nullptr && !cam_cut && (s.io.flags & TF2SR_FG_RESET) == 0) ++g.win.fsr_disc;
		fsr::sr_gen = s.io.generation;
		if (hdl.valid) fsr::hud_gen = hdl.io.generation;
		// 0.4.0: debug inputs (F7 A/B)
		ID3D11Texture2D *in_depth = s.io.depth, *in_mv = s.io.motion_vectors;
		if (g_cfg.fsr_dbg_a || g_cfg.fsr_dbg_ab)
		{
			const int fl = fdbg::frame_flags(reset);
			if ((fl & 3) && fdbg::ensure(s.io.render_width, s.io.render_height))
			{
				if (fl & 1) in_mv = fdbg::zero_mv;
				if (fl & 2) in_depth = fdbg::flat_depth;
			}
		}

		// GPU cut guard, part 1: diff R(k-1) vs R(k) before FSR (the counter is read by the select pass after FSR)
		const bool guard = pair && !reset && ensure_blend() && cut_guard_usable() && g.real_srv[slot_prev] && g.real_srv[slot_cur] && g.out.uav;
		bool guard_ran = false;
		ID3D11ShaderResourceView *srvs[2] = { pair ? g.real_srv[slot_prev] : g.real_srv[slot_cur], g.real_srv[slot_cur] };
		if (guard && write_params(true)) guard_ran = run_cs(srvs, true, true, nullptr);

		const double t0 = qpc_ms();
		tf2fg::PrepareParams pp;
		pp.depth = in_depth;
		pp.motionVectors = in_mv;
		pp.jitterX = std::isfinite(s.io.jitter_x_px) ? s.io.jitter_x_px : 0.0f;
		pp.jitterY = std::isfinite(s.io.jitter_y_px) ? s.io.jitter_y_px : 0.0f;
		pp.renderWidth = s.io.render_width; pp.renderHeight = s.io.render_height;
		pp.cameraNear = cn; pp.cameraFar = cf; pp.cameraFovY = cv;
		pp.frameTimeDeltaMs = static_cast<float>(g.last_dt > 0.0 && g.last_dt < 1000.0 ? g.last_dt : 0.0);
		pp.frameID = k;
		pp.reset = reset;
		tf2fg::Status st = tf2fg::prepare(fsr::ctx, pp);
		if (st == tf2fg::Status::Ok)
		{
			tf2fg::DispatchParams dp;
			dp.currentBackBuffer = g.real[slot_cur];
			dp.hudless = hdl.valid ? hdl.io.texture : nullptr;
			dp.output = g.out.tex;
			dp.transfer = tf2fg::Transfer::Srgb;
			dp.reset = reset;
			st = tf2fg::dispatch(fsr::ctx, dp);
		}
		push(g.win.fsr_cpu, static_cast<float>(qpc_ms() - t0));
		if (st != tf2fg::Status::Ok)
		{
			++g.win.fsr_err; ++fsr::status_err_total; fsr::need_reset = true;
			if (fsr::status_err_total <= 20) logf("fsr: frame %llu %s returned %s - no G%s", k, "prepare/dispatch", tf2fg::statusName(st), tf2fg::isFailed(fsr::ctx) ? " (context failed)" : "");
			return false;
		}
		++g.win.fsr_disp; ++fsr::disp_total;
		fsr::need_reset = false;
		fsr::last_disp = k; fsr::last_had_hud = hdl.valid;
		double gpu_ms = 0.0;
		if (g_cfg.fsr_timing && tf2fg::lastGpuTimeMs(fsr::ctx, &gpu_ms) && std::isfinite(gpu_ms) && gpu_ms >= 0.0 && gpu_ms < 1000.0) push(g.win.fsr_gpu, static_cast<float>(gpu_ms));
		if (reset) { ++g.win.fsr_reset; ++fsr::reset_total; return false; }   // output = R(k): nothing to interpolate, no G
		if (!pair) { ++g.win.fsr_nopair; return false; }
		// GPU cut guard, part 2: gen = cur where the diff pass counted a cut
		if (guard_ran) run_cs(srvs, false, true, g.select_cs);
		++fsr::g_total;
		return true;
	}

	// ------------------------------------------------------------------ state machine (frame boundary = present event)
	// MaxFrameLatency > 0: while ACTIVE the device's maximum frame latency (which counts presents, and FG doubles them)
	// is set to that value; the game's own value is put back whenever FG leaves ACTIVE and at device destruction.
	void apply_latency(bool fg_active)
	{
		if (g_cfg.max_frame_latency <= 0 || !g.dev || fg_active == g.latency_changed) return;
		IDXGIDevice1 *d1 = nullptr;
		if (FAILED(g.dev->QueryInterface(__uuidof(IDXGIDevice1), reinterpret_cast<void **>(&d1))) || !d1) return;
		if (fg_active && !g.latency_changed)
		{
			UINT cur = 0;
			if (SUCCEEDED(d1->GetMaximumFrameLatency(&cur)) && cur > 0 && SUCCEEDED(d1->SetMaximumFrameLatency(static_cast<UINT>(g_cfg.max_frame_latency))))
			{
				g.orig_latency = cur; g.latency_changed = true;
				logf("max frame latency %u -> %d while FG is active", cur, g_cfg.max_frame_latency);
			}
		}
		else if (!fg_active && g.latency_changed)
		{
			d1->SetMaximumFrameLatency(g.orig_latency);
			g.latency_changed = false;
			logf("max frame latency restored to %u", g.orig_latency);
		}
		d1->Release();
	}

	bool update_state(uint64_t k)
	{
		// Mode 3 needs FL 11_1: known as soon as the device exists (RaiseFeatureLevel=1 requests it at device creation)
		if (g_cfg.mode == 3 && g.dev && !fsr::unavailable) fsr_fl_ok();
		const bool eff = effective_enable();
		const bool fsr_blocked = g_cfg.mode == 3 && fsr::unavailable && g_cfg.enable && menu::value == 1;
		if (!fsr_blocked && g.eff_logged == 2) g.eff_logged = 0;   // the owner switched FG off: log "restart required" again next time
		if (g.eff_logged < 0 || eff != (g.eff_logged == 1) || (fsr_blocked && g.eff_logged != 2))
		{
			logf("frame generation %s (Enable=%d, tf2nr_fsr_fg=%d%s%s%s%s%s%s)", eff ? "ENABLED" : "disabled", g_cfg.enable, menu::value,
				g.err_disabled ? ", DXGI error" : "", g.exc_disabled ? ", exception" : "", g.foreign_presenter ? ", foreign presenter" : "",
				g.foreign_render ? ", present hook rendered inside our Present" : "", fsr_blocked ? ", FSR unavailable: " : "", fsr_blocked ? fsr::why : "");
			if (fsr_blocked && g.fl < D3D_FEATURE_LEVEL_11_1)
				logf("FRAME GENERATION (FSR) IS OFF - RESTART REQUIRED: FSR frame interpolation needs the game's D3D11 device at feature level 11_1, this session's device is 0x%x. "
					"Set [TF2FG] RaiseFeatureLevel=1 (the default) in ReShade.ini and restart the game.", static_cast<unsigned>(g.fl));
			g.eff_logged = eff ? 1 : (fsr_blocked ? 2 : 0);
		}
		++g.state_frames;
		// 0.4.0: a pacer error switches presenter 2 off for the session (pass-through; the pacer already detached the visual)
		if (xa::thread && xa::X.error.load() != 0 && !xa::disabled_why)
		{
			xa::disabled_why = xa::pacer_err_name(xa::X.error.load());
			xa::teardown(2, "pacer error");
			logf("presenter 2 OFF for this session after a pacer error (%s) - frame generation passes through", xa::disabled_why);
		}
		const int pres = presenter_now();
		static int pres_logged = -2;
		if (pres != pres_logged)
		{
			logf("presenter: %s%s%s", pres == 2 ? "2 (xa + DComp: G and R presented from the display adapter)" : (pres == 1 ? "1 (in-queue native R Present)" : "none - pass-through"),
				pres == 0 ? " - " : "", pres == 0 ? pass_reason() : "");
			pres_logged = pres;
		}
		if (!eff)
		{
			if (g.st != st_t::off) { logf("state %s -> OFF", st_name(g.st)); g.st = st_t::off; g.state_frames = 0; }
			apply_latency(false);
			fsr::need_reset = true;
			fsr::sr_miss_run = 0;
			if (fsr::ctx && g.state_frames > 600) fsr_destroy("frame generation OFF for 600 frames");   // 0.3.1: free its textures
			// 0.4.0: FG OFF: detach + stop the pacer + release the surfaces at once, the D3D12 devices after 600 frames
			if (xa::thread || xa::builder || xa::S.ok) xa::teardown(1, "frame generation OFF");
			if (xa::D.ok && g.state_frames > 600) xa::teardown(2, "frame generation OFF for 600 frames");
			return false;
		}
		bool vsync_check = false;
		if (g.st == st_t::off)
		{
			g.st = st_t::warmup; g.good_run = 0; g.need = g_cfg.warmup; g.state_frames = 0; vsync_check = true;
			g.fps_off_ema = 0.0; g.off_n = 0;
			logf("state OFF -> WARMUP (%d good frames)", g.need);
		}

		// V-Sync (videoconfig.txt): at WARMUP entry and every 1800 frames
		if (!g_cfg.ignore_vsync && (vsync_check || k >= g.vsync_checked_frame + 1800))
		{
			const int v = read_vsync_mode();
			g.vsync_checked_frame = k;
			if (v != g.vsync_mode) logf("videoconfig: setting.mat_vsync_mode = %d%s", v, v > 0 ? " (V-Sync ON: frame generation suspended, IgnoreVsync=1 overrides)" : (v < 0 ? " (unknown: not checked)" : " (V-Sync off)"));
			g.vsync_mode = v;
		}

		// occlusion: test once every 15 frames while occluded (native Present with DXGI_PRESENT_TEST shows nothing)
		if (g.occluded && (k % 15) == 0 && g.native_sc)
		{
			const HRESULT hr = native_present(DXGI_PRESENT_TEST);
			if (hr == S_OK) { g.occluded = false; logf("no longer occluded"); }
		}

		const bool gameplay = g.fk.main && g.fk.epost;
		const bool fps_valid = g.fps_ema > 0.0;
		// FG cost ratio (predicts the FG-on fps before resuming). One sample per active period: the mean base fps over
		// active frames 30..300 (the first ~4 s) against the FG-off fps EMA at activation (the non-active period right
		// before: adjacent in time, so the scene's own drift matters least). The END of an active period is never used:
		// periods end on fps dips, which would bias the ratio low and lock FG out. Samples are smoothed 50/50 and capped
		// to [0.3, 0.95] (FG is never assumed free).
		if (fps_valid && gameplay && g.state_frames > 10)
		{
			const bool on = g.st == st_t::active;
			if (!on) { g.fps_off_ema = g.fps_off_ema <= 0.0 ? g.fps_ema : g.fps_off_ema + (g.fps_ema - g.fps_off_ema) * 0.05; ++g.off_n; }
			else
			{
				++g.on_n;
				if (g.on_n >= 30 && g.on_n <= 300) g.on_sum += g.fps_ema;
				g.fps_on_ema = g.fps_on_ema <= 0.0 ? g.fps_ema : g.fps_on_ema + (g.fps_ema - g.fps_on_ema) * 0.05;   // log only
				if (g.on_n == 300 && g.fps_off_at_start > 0.0)
				{
					const double on_mean = g.on_sum / 271.0;
					double r = on_mean / g.fps_off_at_start;
					if (g.ratio_measured) r = 0.5 * g.fg_ratio + 0.5 * r;
					g.fg_ratio = r < 0.3 ? 0.3 : (r > 0.95 ? 0.95 : r);
					if (g.ratio_samples++ < 10)
						logf("FG cost sample: FG-on fps %.1f (active frames 30-300) / FG-off fps %.1f (before activation) -> ratio %.3f (smoothed, capped [0.3, 0.95])", on_mean, g.fps_off_at_start, g.fg_ratio);
					g.ratio_measured = true;
				}
			}
		}
		const double predicted = g.fps_ema * g.fg_ratio;   // FG-on fps expected if FG started now (meaningful while not ACTIVE)
		// 0.4.0: presenter 2 shows G and R as two refreshes of the panel: a base rate above refresh / 2 cannot be shown
		double max_fps = g_cfg.max_fps;
		if (pres == 2 && xa::topo.refresh_hz > 1.0 && xa::topo.refresh_hz * 0.5 < max_fps)
		{
			max_fps = xa::topo.refresh_hz * 0.5;
			static bool cap_logged = false;
			if (!cap_logged) { cap_logged = true; logf("presenter 2: MaxBaseFps %.1f capped to %.2f (= %.0f Hz / 2: a G/R pair needs two refreshes)", g_cfg.max_fps, max_fps, xa::topo.refresh_hz); }
		}
		if (fps_valid && g.fps_ema < g_cfg.min_fps) ++g.low_run; else g.low_run = 0;
		if (fps_valid && g.fps_ema > max_fps) ++g.high_run; else g.high_run = 0;
		const char *bad = nullptr;
		if (!gameplay) bad = "menu/loading frame (no main pass or engine_post)";
		else if (g_cfg.require_tsaa && !g.fk.tsaa) bad = "no TSAA anchor: this frame has no TSAA pass (DLSS SR and in-game TSAA are off), so pre_dlss/post_dlss do not exist (RequireTSAA=1)";
		else if (g_cfg.mode == 3 && fsr::sr_miss_run >= 30) bad = "no DLSS SR inputs: FSR frame generation needs DLSS Super Resolution (tf2-dlss-sr depth + motion vectors)";
		else if (g_cfg.mode == 3 && !fsr::ctx) bad = "FSR context not ready (created outside ACTIVE)";
		else if (pres == 0) bad = pass_reason();
		else if (pres == 2 && !xa_ready()) bad = xa_not_ready_reason();
		else if (!g_cfg.ignore_vsync && g.vsync_mode > 0) bad = "V-Sync is on (videoconfig mat_vsync_mode)";
		else if (g.occluded) bad = "occluded";
		else if (k < g.err_hold_until) bad = "DXGI error hold";
		else if (!g.native_sc || !g.bb || !g.rt) bad = "no swap chain / runtime";
		else if (g_cfg.cost_ab > 0 && costab::phase == 0) bad = "cost A/B: OFF phase (XaCostAB)";
		else if (g.st == st_t::active && g.low_run >= 15) bad = "base fps below MinBaseFps";
		else if (g.st == st_t::active && g.high_run >= 15) bad = "base fps above MaxBaseFps";
		else if (g.st != st_t::active && fps_valid && (predicted < g_cfg.min_fps * 1.1 || predicted > max_fps))
			bad = "predicted FG-on fps outside [MinBaseFps x 1.1, MaxBaseFps]";

		if (g.st == st_t::active)
		{
			if (bad)
			{
				g.st = st_t::suspended; g.reason = bad; g.good_run = 0; ++g.win.suspends; g.state_frames = 0;
				g.fps_off_ema = 0.0; g.off_n = 0;
				const bool fps = strstr(bad, "fps") != nullptr;
				if (fps) { g.fps_backoff = g.fps_backoff == 0 ? g_cfg.resume * 2 : std::min(g.fps_backoff * 2, 1200); g.active_since_fps_suspend = 0; }
				g.need = fps ? g.fps_backoff : g_cfg.resume;
				logf("state ACTIVE -> SUSPENDED (%s; base fps %.1f; resume after %d good frames)", bad, g.fps_ema, g.need);
			}
			else if (++g.active_since_fps_suspend > 600) g.fps_backoff = 0;
		}
		else
		{
			if (bad) { if (g.st == st_t::suspended) g.reason = bad; g.good_run = 0; }
			else if (++g.good_run >= g.need)
			{
				logf("state %s -> ACTIVE (base fps %.1f, predicted FG-on fps %.1f = x %.3f %s, %s)", st_name(g.st), g.fps_ema, predicted, g.fg_ratio,
					g.ratio_measured ? "measured" : "default", pres == 2 ? "presenter 2" : (inject_kind() == A_PRE ? "presenter 1, anchor pre_dlss" : "presenter 1"));
				g.st = st_t::active; g.low_run = g.high_run = 0; g.state_frames = 0;
				g.fps_on_ema = 0.0; g.on_n = 0; g.on_sum = 0.0;
				g.fps_off_at_start = g.off_n >= 40 ? g.fps_off_ema : 0.0;   // no sample without a proper FG-off reference
			}
		}
		apply_latency(g.st == st_t::active);
		const bool on = g.st == st_t::active && bad == nullptr;
		if (!on) fsr::need_reset = true;   // FSR history restarts at the next (re)activation
		return on;
	}

	float avg(const std::vector<float> &v) { if (v.empty()) return NAN; double s = 0; for (float x : v) s += x; return static_cast<float>(s / static_cast<double>(v.size())); }
	float pct(std::vector<float> v, double p)
	{
		if (v.empty()) return NAN;
		const size_t i = std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5));
		std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(i), v.end());
		return v[i];
	}

	cpu_snap g_win_cpu;   // 0.4.1: CPU sample at the start of the tf2-fg window
	void log_window(double now)
	{
		window_t &w = g.win;
		{
			const cpu_snap c = cpu_now();
			double gp = NAN, sp = NAN;
			if (g_win_cpu.sys_total) cpu_split(g_win_cpu, c, &gp, &sp);
			g_win_cpu = c;
			logf("   machine: game process CPU %.1f %% of all logical processors, whole system %.1f %% (rest of the system %.1f %%) | game window in the foreground %.0f %% of frames",
				gp, sp, sp - gp, w.frames ? 100.0 * static_cast<double>(w.fore) / static_cast<double>(w.frames) : 0.0);
		}
		fcsv::flush();
		const double secs = (now - w.start_ms) / 1000.0;
		const double fps = secs > 0.0 ? static_cast<double>(w.frames) / secs : 0.0;
		const double out = secs > 0.0 ? static_cast<double>(w.frames + w.native_presents) / secs : 0.0;
		const double ppf = w.frames ? static_cast<double>(w.frames + w.native_presents) / static_cast<double>(w.frames) : 0.0;
		const uint64_t r_all = w.r_pre + w.r_post + w.r_dead + w.r_late;
		const double ppf_fg = w.fg_frames ? static_cast<double>(w.fg_frames + r_all) / static_cast<double>(w.fg_frames) : 0.0;
		logf("window %llu frames (%.1f s): base fps %.2f, output %.2f presents/s | state %s%s%s | FG frames %llu (gameplay %llu, TSAA %llu) | presents per game frame %.3f (FG frames %.3f) | "
			"R presented %llu (pre %llu post %llu deadline %llu late %llu) dropped %llu | occluded %llu, DXGI errors %llu, suspends %llu | native Present CPU avg %.3f ms max %.2f ms | menu fg=%d sr=%d nr=%d",
			w.frames, secs, fps, out, st_name(g.st), g.st == st_t::suspended ? ": " : "", g.st == st_t::suspended ? g.reason : "", w.fg_frames, w.gameplay, w.tsaa,
			ppf, ppf_fg, r_all, w.r_pre, w.r_post, w.r_dead, w.r_late, w.r_dropped, w.occluded, w.dxgi_err, w.suspends,
			w.native_presents ? w.present_cpu_ms / static_cast<double>(w.native_presents) : 0.0, w.present_cpu_max, menu::value, menu::sr_mode, menu::nr);
		logf("   cuts: CPU (no G) %llu, GPU guard (gen = cur) %llu of %llu read back (changed px avg %.1f %% max %.1f %%) | present-hook events inside our Present %llu | debug flushes %llu | fps FG-on %.1f FG-off %.1f ratio %.3f%s | work level %d",
			w.cut_cpu, w.cut_gpu, w.cut_samples, w.cut_samples ? 100.0 * w.cut_frac_sum / static_cast<double>(w.cut_samples) : 0.0, 100.0 * w.cut_frac_max,
			w.foreign, w.flushes, g.fps_on_ema, g.fps_off_ema, g.fg_ratio, g.ratio_measured ? "" : " (default)", g_level);
		if (g_cfg.mode == 3 && (w.sr_polls || w.fsr_disp || w.fsr_nosr))
			logf("   fsr: dispatches %llu (reset %llu: SR camera cut %llu, library rule only %llu; no pair %llu) -> G %llu | passthrough: no SR inputs %llu (convention refused %llu), no camera %llu (held %llu), no context %llu | HUD-less missing %llu | "
				"library status errors %llu, library ERROR lines %llu (session %llu) | SR export valid %llu/%llu, HUD-less valid %llu/%llu | "
				"OF+FI GPU ms avg %.3f p50 %.3f p95 %.3f (n %zu) | CPU prepare+dispatch ms avg %.3f p95 %.3f | ctx %s (creates %d, failures %d)",
				w.fsr_disp, w.fsr_reset, w.fsr_camcut, w.fsr_disc, w.fsr_nopair, w.fsr_disp - w.fsr_reset - w.fsr_nopair, w.fsr_nosr, w.sr_conv, w.fsr_nocam, w.fsr_camfb, w.fsr_noctx, w.fsr_nohud,
				w.fsr_err, w.fsr_lib_err, fsr::lib_err_total, w.sr_valid, w.sr_polls, w.hud_valid, w.hud_polls,
				avg(w.fsr_gpu), pct(w.fsr_gpu, 0.5), pct(w.fsr_gpu, 0.95), w.fsr_gpu.size(), avg(w.fsr_cpu), pct(w.fsr_cpu, 0.95),
				fsr::ctx ? "live" : (fsr::unavailable ? "UNAVAILABLE" : "none"), fsr::creates, fsr::failures);
		if (prof_on())
			logf("   gpu (ms, from G = back buffer ready for the proxied present): G->R avg %.2f p50 %.2f p95 %.2f | R->G avg %.2f p50 %.2f p95 %.2f | D avg %.2f p95 %.2f | "
				"anchors after G: pre_dlss %.2f post_dlss %.2f deadline %.2f, frame end %.2f | DLSS+NR chunk (pre->post) avg %.2f p95 %.2f | G present done %.2f | G build %.3f | "
				"samples %llu (g2r %zu r2g %zu d %zu), disjoint %llu, lost %llu | inject at %s",
				avg(w.g2r), pct(w.g2r, 0.5), pct(w.g2r, 0.95), avg(w.r2g), pct(w.r2g, 0.5), pct(w.r2g, 0.95), avg(w.d), pct(w.d, 0.95),
				avg(w.pre), avg(w.post), avg(w.dead), avg(w.send), avg(w.chunk), pct(w.chunk, 0.95), avg(w.fp), avg(w.gbuild),
				w.samples, w.g2r.size(), w.r2g.size(), w.d.size(), w.disjoint, w.lost, anchor_name(inject_kind()));
		logf("   cpu (ms): frame avg %.2f p95 %.2f | proxied Present avg %.2f p95 %.2f | native R Present avg %.2f p50 %.2f p95 %.2f (calls %zu, WAS_STILL_DRAWING %llu, R on a retry %llu) | frame start->pre_dlss avg %.2f | DLSS+NR evaluate (pre->post) avg %.2f p95 %.2f",
			avg(w.c_frame), pct(w.c_frame, 0.95), avg(w.c_proxy), pct(w.c_proxy, 0.95), avg(w.c_rpres), pct(w.c_rpres, 0.5), pct(w.c_rpres, 0.95), w.c_rpres.size(), w.wsd, w.retries_ok,
			avg(w.c_to_pre), avg(w.c_dlss), pct(w.c_dlss, 0.95));
		if (presenter_now() == 2 || xa::S.ok || w.xa_jobs)
			logf("   presenter2 (render thread): jobs issued %llu (with G %llu, R only %llu), slot busy %llu (G built but not shown %llu), detach requests %llu, minimized frames %llu | "
			     "our reshade_present CPU ms avg %.3f p50 %.3f p95 %.3f | pacer %s%s, visual %s | session: presented %llu, pacer-skipped jobs %llu, attaches %llu, detaches %llu | %s",
				w.xa_jobs, w.xa_jobs_g, w.xa_jobs - w.xa_jobs_g, w.xa_busy, w.xa_gdrop, w.xa_detreq, w.xa_min, avg(w.c_rp), pct(w.c_rp, 0.5), pct(w.c_rp, 0.95),
				xa::thread ? (xa::X.running.load() ? "running" : "starting/stopped") : "not running", xa::X.error.load() ? " (ERROR)" : "", xa::X.attached.load() ? "ATTACHED" : "detached",
				xa::X.presented.load(), xa::X.skipped.load(), xa::X.attaches.load(), xa::X.detaches.load(), xa::builder ? "(builder running)" : xa::evidence.c_str());
		else if (!w.c_rp.empty())
			logf("   our reshade_present CPU ms avg %.3f p50 %.3f p95 %.3f", avg(w.c_rp), pct(w.c_rp, 0.5), pct(w.c_rp, 0.95));
		if (xa::stalls_total || xa::stalling || xa::pend_level)
			logf("   presenter2 watchdog: stalls this session %llu%s%s", static_cast<unsigned long long>(xa::stalls_total), xa::stalling ? ", STALLED pacer not exited yet" : "", xa::pend_level ? ", a teardown is deferred" : "");
		if (g_cfg.inject_at == 0)
		{
			// experimental auto anchor: the one whose offset from G is closest to D/2 + bias
			const float d = avg(w.d);
			if (std::isfinite(d))
			{
				const float target = d * 0.5f + g_cfg.pace_bias_ms;
				int best = A_PRE; float best_err = 1e9f;
				const struct { int k; float off; } c[3] = { { A_PRE, avg(w.pre) }, { A_POST, avg(w.post) }, { A_DEAD, avg(w.dead) } };
				for (const auto &x : c) if (std::isfinite(x.off) && std::fabs(x.off - target) < best_err) { best_err = std::fabs(x.off - target); best = x.k; }
				if (best != g.anchor_auto) logf("   auto anchor: %s -> %s (target %.2f ms = D/2 + bias)", anchor_name(g.anchor_auto), anchor_name(best), target);
				g.anchor_auto = best;
			}
		}
		w.reset(now);
	}

	// ------------------------------------------------------------------ events
	// ReShade's own version (the module that exports ReShadeRegisterAddon). RaiseFeatureLevel relies on ReShade 6.8's
	// D3D11CreateDevice hook passing a ppDevice = NULL support probe straight through (verified on 6.8.0 only).
	bool reshade_version(unsigned v[4])
	{
		HMODULE m = reshade::internal::get_reshade_module_handle();
		if (!m) return false;
		wchar_t path[MAX_PATH] = {};
		if (!GetModuleFileNameW(m, path, MAX_PATH)) return false;
		DWORD h = 0; const DWORD n = GetFileVersionInfoSizeW(path, &h);
		if (n == 0 || n > (1u << 20)) return false;
		std::vector<uint8_t> buf(n);
		if (!GetFileVersionInfoW(path, 0, n, buf.data())) return false;
		VS_FIXEDFILEINFO *fi = nullptr; UINT len = 0;
		if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void **>(&fi), &len) || !fi || len < sizeof(VS_FIXEDFILEINFO)) return false;
		v[0] = HIWORD(fi->dwFileVersionMS); v[1] = LOWORD(fi->dwFileVersionMS); v[2] = HIWORD(fi->dwFileVersionLS); v[3] = LOWORD(fi->dwFileVersionLS);
		return true;
	}

	bool on_create_device(device_api api, uint32_t &api_version)
	{
		FG_TRY
		static bool first_d3d11 = true;   // consumed by the first D3D11 device only (the game's), not by other APIs
		const bool is_first = api == device_api::d3d11 && first_d3d11;
		if (api == device_api::d3d11) first_d3d11 = false;
		logf("create_device: api %d, level 0x%x%s", static_cast<int>(api), api_version, is_first ? " (first D3D11 device = the game's)" : "");
		if (!is_first || !g_cfg.raise_fl) return false;
		if (!g_cfg.enable || g_cfg.mode != 3)
		{
			// 0.3.1: Enable and Mode are read once at start, so FSR FG cannot be switched on in this session: keep the game's level
			logf("RaiseFeatureLevel: not applied - [TF2FG] Enable=%d Mode=%d (FSR frame generation cannot run this session; both are read at start) - the game's device keeps level 0x%x",
				g_cfg.enable, g_cfg.mode, api_version);
			return false;
		}
		if (api_version >= 0xb100) { logf("RaiseFeatureLevel: level 0x%x already >= 11_1 - unchanged", api_version); return false; }
		unsigned rv[4] = {};
		if (!reshade_version(rv) || rv[0] != 6 || rv[1] != 8)
		{
			logf("RaiseFeatureLevel: ReShade version %u.%u.%u.%u is not the tested 6.8.x - left unchanged", rv[0], rv[1], rv[2], rv[3]);
			return false;
		}
		static thread_local bool busy = false;
		if (busy) return false;
		busy = true;
		HRESULT hr = E_FAIL;
		D3D_FEATURE_LEVEL got = static_cast<D3D_FEATURE_LEVEL>(0);
		try
		{
			using create_fn = HRESULT(WINAPI *)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
			HMODULE m = GetModuleHandleW(L"d3d11.dll");
			const auto fn = m ? reinterpret_cast<create_fn>(GetProcAddress(m, "D3D11CreateDevice")) : nullptr;
			const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_1;
			// ppDevice = NULL: a support probe. ReShade 6.8's hook passes it straight through (no create_device event).
			if (fn) hr = fn(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &want, 1, D3D11_SDK_VERSION, nullptr, &got, nullptr);
		}
		catch (...) { hr = E_FAIL; }
		busy = false;
		if (SUCCEEDED(hr) && got == D3D_FEATURE_LEVEL_11_1)
		{
			api_version = 0xb100;
			logf("RaiseFeatureLevel: ReShade %u.%u.%u.%u, 11_1 supported on the default adapter (probe 0x%08x) -> the game's device is requested at 11_1 (the game is still told 0x%x)",
				rv[0], rv[1], rv[2], rv[3], static_cast<unsigned>(hr), 0xb000u);
			return true;
		}
		logf("RaiseFeatureLevel: 11_1 NOT supported (probe 0x%08x, level 0x%x) - left unchanged", static_cast<unsigned>(hr), static_cast<unsigned>(got));
		return false;
		FG_CATCH(false)
	}

	void log_device(device *d)
	{
		auto *dev = reinterpret_cast<ID3D11Device *>(d->get_native());
		if (!dev) return;
		const D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
		const UINT flags = dev->GetCreationFlags();
		char adapter[160] = "?";
		UINT latency = 0;
		IDXGIDevice *dxgi = nullptr;
		if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi))) && dxgi)
		{
			IDXGIAdapter *ad = nullptr;
			if (SUCCEEDED(dxgi->GetAdapter(&ad)) && ad)
			{
				DXGI_ADAPTER_DESC ds = {};
				if (SUCCEEDED(ad->GetDesc(&ds)))
					snprintf(adapter, sizeof(adapter), "%ls (vendor %04x device %04x, %llu MB dedicated)", ds.Description, ds.VendorId, ds.DeviceId,
						static_cast<unsigned long long>(ds.DedicatedVideoMemory / (1024 * 1024)));
				ad->Release();
			}
			IDXGIDevice1 *d1 = nullptr;
			if (SUCCEEDED(dxgi->QueryInterface(__uuidof(IDXGIDevice1), reinterpret_cast<void **>(&d1))) && d1) { d1->GetMaximumFrameLatency(&latency); d1->Release(); }
			dxgi->Release();
		}
		logf("D3D11 device %p: feature level 0x%x (%s), creation flags 0x%x, max frame latency %u, adapter %s", dev, static_cast<unsigned>(fl),
			fl >= D3D_FEATURE_LEVEL_11_1 ? "11_1+ : 64 UAV slots" : "11_0 : 8 CS UAV slots", flags, latency, adapter);
	}

	void on_init_device(device *d)
	{
		FG_TRY
		if (d && d->get_api() == device_api::d3d12)
		{
			// 0.4.0 wrapping evidence: a D3D12 device ReShade wraps raises init_device - during OUR create (thread-local flag)
			// that means presenter 2's device is a ReShade proxy (its native pointer is captured here)
			++xa::n_init_d3d12;
			const bool ours = xa::t_creating != 0;
			if (ours) { xa::t_cap_native = d->get_native(); xa::t_cap_dev = d; xa::last_cap_dev.store(d); }
			if (ours || xa::n_init_d3d12 <= 12)
				logf("init_device: D3D12 device %p (native %llx)%s", d, static_cast<unsigned long long>(d->get_native()),
					ours ? " - raised DURING presenter 2's own device creation: ReShade wrapped it" : " (another component's device)");
			return;
		}
		if (!d || d->get_api() != device_api::d3d11) return;
		if (!g.first_d3d11_device_logged) { g.first_d3d11_device_logged = true; log_device(d); }
		FG_CATCH()
	}

	void release_device_objects()
	{
		apply_latency(false);
		fsr_destroy("device released");
		fsr::sr_mod = nullptr; fsr::hud_mod = nullptr; fsr::get_sr = nullptr; fsr::get_hud = nullptr;   // re-resolve with the next device
		fsr::sr_gen = fsr::hud_gen = ~0ull; fsr::cam_ok = false; fsr::sr_miss_run = 0;
		if (fsr::unavailable && g.fl < D3D_FEATURE_LEVEL_11_1) { fsr::unavailable = false; fsr::why = ""; }   // the next device may be 11_1
		dump::release();
		fdbg::release();
		release_queries();
		release_resources();
		release_blend();
		safe_release(g.imm1); safe_release(g.dev1);
		safe_release(g.imm);
		g.dev = nullptr; g.rdev = nullptr;
		g.pending = false; g.r_in_bb = false; g.bb_bound = false;
		g.res_retry_frame = 0;
		{
			std::lock_guard lock(g_tag_mx);   // pipeline handles of the old device must not tag new pipelines at reused addresses
			g_tags.clear();
		}
		menu::rearm();
	}

	// Destroy events are NEVER skipped by the re-entrancy guard: a stale swap chain / back buffer pointer would be fatal.
	void on_destroy_device(device *d)
	{
		try
		{
			if (d == nullptr) return;
			if (d->get_api() == device_api::d3d12)
			{
				if (t_depth == 0 && !t_in_native && (xa::n_init_d3d12 <= 12 || d == xa::last_cap_dev.load())) logf("destroy_device: D3D12 device %p (native %llx)%s", d, static_cast<unsigned long long>(d->get_native()),
					d == xa::last_cap_dev.load() ? " - presenter 2's refused (ReShade-seen) device" : "");
				return;
			}
			if (d != g.rdev) return;
			xa::teardown(2, "game device destroyed", true);   // builder / pacer joined (bounded; not inside a present event) before anything goes away
			release_device_objects();
			g.rsc = nullptr; g.native_sc = nullptr; g.bb = 0; ++g.sc_gen; g.rt = nullptr;
			if (t_depth == 0 && !t_in_native) logf("device destroyed - released everything");
		}
		catch (...) { g.native_sc = nullptr; g.bb = 0; g.pending = false; g.exc_disabled = true; }
	}

	// init_swapchain always refreshes the native pointers (not guarded); only logging is skipped inside our own Present.
	void on_init_swapchain(swapchain *sc, bool resize)
	{
		try
		{
			if (!sc) return;
			device *d = sc->get_device();
			if (d && d->get_api() != device_api::d3d11)
			{
				// 0.4.0: presenter 2's composition swap chain must never be seen by ReShade (unwrapped factory)
				++xa::n_foreign_sc;
				logf("init_swapchain: NON-D3D11 swap chain %p (api %d, native %llx, device %p)%s", sc, static_cast<int>(d->get_api()), static_cast<unsigned long long>(sc->get_native()), d,
					xa::t_creating_sc ? " - raised DURING presenter 2's composition swap chain creation: ReShade WRAPPED it" : "");
				if (xa::t_creating_sc) ++xa::n_sc_during_ours;
				return;
			}
			if (!d) return;
			const bool quiet = t_in_native || t_depth != 0;
			if (g.rsc != nullptr && g.rsc != sc) { if (!quiet) logf("init_swapchain: a second swap chain %p ignored (tracking %p)", sc, g.rsc); return; }
			if (g.rdev != d)
			{
				if (g.rdev) release_device_objects();
				g.rdev = d;
				g.dev = reinterpret_cast<ID3D11Device *>(d->get_native());
				if (g.dev)
				{
					g.fl = g.dev->GetFeatureLevel();
					g.dev_flags = g.dev->GetCreationFlags();
					g.dev->GetImmediateContext(&g.imm);
					if (FAILED(g.dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void **>(&g.dev1)))) g.dev1 = nullptr;
					if (g.imm && FAILED(g.imm->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void **>(&g.imm1)))) g.imm1 = nullptr;
				}
			}
			g.rsc = sc;
			g.native_sc = reinterpret_cast<IDXGISwapChain *>(sc->get_native());
			g.bb = sc->get_current_back_buffer().handle;
			++g.sc_gen;
			g.pending = false; g.r_in_bb = false; g.bb_bound = false;
			g.res_retry_frame = 0;
			if (quiet) return;
			DXGI_SWAP_CHAIN_DESC sd = {};
			if (g.native_sc && SUCCEEDED(g.native_sc->GetDesc(&sd)))
				logf("init_swapchain%s: native %p, %ux%u fmt %u, BufferCount %u, SwapEffect %u, Flags 0x%x, Windowed %d | device FL 0x%x | back buffer %llx",
					resize ? " (resize)" : "", g.native_sc, sd.BufferDesc.Width, sd.BufferDesc.Height, static_cast<unsigned>(sd.BufferDesc.Format), sd.BufferCount,
					static_cast<unsigned>(sd.SwapEffect), sd.Flags, sd.Windowed, static_cast<unsigned>(g.fl), g.bb);
			static bool first = true;
			scan_modules(first);
			if (first || !resize) log_present_path(g.native_sc);
			first = false;
			if (g_live && g.native_sc && sd.BufferDesc.Width) xa::evaluate_topology(g.native_sc, g.dev, sd.BufferDesc.Width, sd.BufferDesc.Height);
		}
		catch (...) { g.exc_disabled = true; g.pending = false; logf("exception in init_swapchain - frame generation OFF"); }
	}

	void on_destroy_swapchain(swapchain *sc, bool resize)
	{
		try
		{
			if (sc != g.rsc || sc == nullptr) return;
			xa::teardown(1, resize ? "game swap chain resize" : "game swap chain destroyed");   // pacer joined, visual detached, surfaces released
			if (g.pending) { ++g.win.r_dropped; ++g.drop_total; }
			g.pending = false; g.r_in_bb = false; g.bb_bound = false;
			safe_release(g.bb_rtv);   // a view holds a back-buffer reference: ResizeBuffers fails while one is alive
			g.native_sc = nullptr; g.bb = 0; ++g.sc_gen;
			fsr::need_reset = true;
			dump::stage = 0;
			if (!resize) { g.rsc = nullptr; release_resources(); }
			if (t_depth == 0 && !t_in_native) logf("destroy_swapchain%s", resize ? " (resize)" : "");
		}
		catch (...) { g.native_sc = nullptr; g.bb = 0; g.pending = false; g.exc_disabled = true; }
	}

	// Only the runtime of OUR swap chain is tracked (a runtime of a second swap chain must not replace it).
	void on_init_effect_runtime(effect_runtime *rt)
	{
		try
		{
			if (!rt) return;
			if (g.native_sc && rt->get_native() != reinterpret_cast<uint64_t>(g.native_sc))
			{
				++xa::n_foreign_rt;
				if (xa::t_creating_sc) ++xa::n_rt_during_ours;
				if (t_depth == 0 && !t_in_native) logf("effect runtime %p ignored (not our swap chain; native %llx)%s", rt, static_cast<unsigned long long>(rt->get_native()),
					(xa::S.msc && rt->get_native() == reinterpret_cast<uint64_t>(xa::S.msc)) || xa::t_creating_sc ? " - it is presenter 2's COMPOSITION swap chain" : "");
				return;
			}
			g.rt = rt;
			if (t_depth == 0 && !t_in_native) logf("effect runtime %p attached", rt);
		}
		catch (...) { g.exc_disabled = true; }
	}
	void on_destroy_effect_runtime(effect_runtime *rt)
	{
		try
		{
			if (rt != g.rt || rt == nullptr) return;
			g.rt = nullptr; g.pending = false; g.r_in_bb = false;
			if (t_depth == 0 && !t_in_native) logf("effect runtime %p detached", rt);
		}
		catch (...) { g.rt = nullptr; g.pending = false; g.exc_disabled = true; }
	}

	void on_init_pipeline(device *, pipeline_layout, uint32_t count, const pipeline_subobject *subobjects, pipeline p)
	{
		if (t_in_native) { ++t_foreign; return; }
		FG_TRY
		if (!subobjects) return;
		for (uint32_t i = 0; i < count; ++i)
		{
			if (subobjects[i].type != pipeline_subobject_type::pixel_shader) continue;
			const auto *sd = static_cast<const shader_desc *>(subobjects[i].data);
			if (!sd || !sd->code || sd->code_size < 32) continue;
			uint32_t tag = 0;
			if (has_str(sd->code, sd->code_size, "CBufTSAA") && has_str(sd->code, sd->code_size, "VelocityBuffer")) tag |= TAG_TSAA;
			if (has_str(sd->code, sd->code_size, "CBufEnginePost") && has_str(sd->code, sd->code_size, "FBTexture")) tag |= TAG_EPOST;
			if (!tag) continue;
			{
				std::lock_guard lock(g_tag_mx);
				g_tags[p.handle] = tag;
			}
			if (tag & TAG_TSAA) logf("pipeline %016llx: TSAA PS %016llx", p.handle, fnv1a(sd->code, sd->code_size));
		}
		FG_CATCH()
	}
	void on_destroy_pipeline(device *, pipeline p)
	{
		try { std::lock_guard lock(g_tag_mx); g_tags.erase(p.handle); }
		catch (...) {}
	}

	// ---- command-stream callbacks (render thread, immediate context). Level 0: return at once. Level 1: gameplay flags
	// only. Level 2: anchors, deadline, timestamps.
	void on_bind_pipeline(command_list *cmd, pipeline_stage stages, pipeline p)
	{
		if (t_in_native) { ++t_foreign; return; }
		if ((stages & pipeline_stage::pixel_shader) == 0 || g_level == 0) return;
		FG_ENTER();
		FG_TRY
		if (!on_imm(cmd)) return;
		g.ps_tag = tag_of(p.handle);
		if (g_level >= 2 && (g.ps_tag & TAG_TSAA) && !g.f.pre) { g.f.pre = true; anchor(A_PRE); }
		FG_CATCH()
	}

	void on_bind_rts(command_list *cmd, uint32_t count, const resource_view *rtvs, resource_view)
	{
		if (t_in_native) { ++t_foreign; return; }
		if (g_level == 0) return;
		FG_ENTER();
		FG_TRY
		if (!on_imm(cmd)) return;
		device *dev = cmd->get_device();
		if (g_level >= 2)
		{
			check_post();
			if (g.pending && g.r_in_bb) inject_r(g.r_kind_first);   // NoWait retry (the back buffer still holds R; the deadline check below wins if this bind is the back buffer)
			// back buffer bound by this bind? (kept across frames: a draw while it is still bound is also a deadline)
			bool bound = false;
			if (dev && rtvs && g.bb)
				for (uint32_t i = 0; i < count && i < 8 && !bound; ++i)
					if (rtvs[i].handle && dev->get_resource_from_view(rtvs[i]).handle == g.bb) bound = true;
			g.bb_bound = bound;
			if (bound) check_dead_resource(g.bb);
		}
		if (!dev || rtvs == nullptr) return;
		if (!g.f.main && count >= 2 && rtvs[0].handle && rtvs[1].handle)
		{
			const resource r0 = dev->get_resource_from_view(rtvs[0]), r1 = dev->get_resource_from_view(rtvs[1]);
			if (r0.handle && r1.handle)
			{
				const auto d0 = dev->get_resource_desc(r0), d1 = dev->get_resource_desc(r1);
				g.f.main = d0.type == resource_type::texture_2d && d1.type == resource_type::texture_2d &&
				           (d0.texture.format == format::r16g16b16a16_typeless || d0.texture.format == format::r16g16b16a16_float) &&
				           (d1.texture.format == format::r16g16_typeless || d1.texture.format == format::r16g16_float) &&
				           d0.texture.width == d1.texture.width && d0.texture.height == d1.texture.height;
				if (g.f.main) { g.f.main_w = d0.texture.width; g.f.main_h = d0.texture.height; }
			}
		}
		FG_CATCH()
	}

	bool on_draw_common(command_list *cmd)
	{
		if (t_in_native) { ++t_foreign; return false; }
		if (g_level == 0) return false;
		FG_ENTER(false);
		FG_TRY
		if (!on_imm(cmd)) return false;
		if (g_level >= 2)
		{
			check_post();
			// the back buffer is still bound from an earlier bind (e.g. from the previous frame) and this draw writes it
			if (g.bb_bound && !g.f.dead && g.bb) { g.f.dead = true; anchor(A_DEAD); }
		}
		const uint32_t tag = g.ps_tag;
		if (tag & TAG_EPOST) g.f.epost = true;
		if (tag & TAG_TSAA) { g.f.tsaa = true; if (g_level >= 2 && !g.f.post) g.f.after_tsaa = true; }   // the TSAA draw executes after all callbacks return
		return false;
		FG_CATCH(false)
	}
	bool on_draw(command_list *cmd, uint32_t, uint32_t, uint32_t, uint32_t) { return on_draw_common(cmd); }
	bool on_draw_indexed(command_list *cmd, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { return on_draw_common(cmd); }
	bool on_indirect(command_list *cmd, indirect_command, resource, uint64_t, uint32_t, uint32_t) { return on_draw_common(cmd); }

	// level-2-only callbacks: post_dlss anchor + deadline on writes into the back buffer
	bool level2_enter()
	{
		if (t_in_native) { ++t_foreign; return false; }
		return g_level >= 2;
	}
	bool on_dispatch(command_list *cmd, uint32_t, uint32_t, uint32_t)
	{
		if (!level2_enter()) return false;
		FG_ENTER(false);
		FG_TRY if (on_imm(cmd)) check_post(); return false; FG_CATCH(false)
	}
	bool on_clear_rt(command_list *cmd, resource_view rtv, const float[4], uint32_t, const rect *)
	{
		if (!level2_enter()) return false;
		FG_ENTER(false);
		FG_TRY
		if (!on_imm(cmd)) return false;
		check_post();
		if (!g.f.dead && g.bb && rtv.handle && cmd->get_device()) check_dead_resource(cmd->get_device()->get_resource_from_view(rtv).handle);
		return false;
		FG_CATCH(false)
	}
	bool on_clear_ds(command_list *cmd, resource_view, const float *, const uint8_t *, uint32_t, const rect *)
	{
		if (!level2_enter()) return false;
		FG_ENTER(false);
		FG_TRY if (on_imm(cmd)) check_post(); return false; FG_CATCH(false)
	}
	bool on_copy_resource(command_list *cmd, resource, resource dst)
	{
		if (!level2_enter()) return false;
		FG_ENTER(false);
		FG_TRY if (on_imm(cmd)) { check_post(); check_dead_resource(dst.handle); } return false; FG_CATCH(false)
	}
	bool on_copy_texture_region(command_list *cmd, resource, uint32_t, const subresource_box *, resource dst, uint32_t, const subresource_box *, filter_mode)
	{
		if (!level2_enter()) return false;
		FG_ENTER(false);
		FG_TRY if (on_imm(cmd)) { check_post(); check_dead_resource(dst.handle); } return false; FG_CATCH(false)
	}
	bool on_resolve(command_list *cmd, resource, uint32_t, const subresource_box *, resource dst, uint32_t, uint32_t, uint32_t, uint32_t, format)
	{
		if (!level2_enter()) return false;
		FG_ENTER(false);
		FG_TRY if (on_imm(cmd)) { check_post(); check_dead_resource(dst.handle); } return false; FG_CATCH(false)
	}
	bool on_copy_buffer_to_texture(command_list *cmd, resource, uint64_t, uint32_t, uint32_t, resource dst, uint32_t, const subresource_box *)
	{
		if (!level2_enter()) return false;
		FG_ENTER(false);
		FG_TRY if (on_imm(cmd)) { check_post(); check_dead_resource(dst.handle); } return false; FG_CATCH(false)
	}
	// UpdateSubresource into the back buffer on the render thread. A deferred-context call on the render thread would also
	// count: harmless (R is presented a little early), while missing an immediate one would let R overwrite game content.
	bool on_update_texture(device *, const subresource_data &, resource dst, uint32_t, const subresource_box *)
	{
		if (!level2_enter()) return false;
		FG_ENTER(false);
		FG_TRY
		if (g.imm && g.render_tid == GetCurrentThreadId() && dst.handle == g.bb) check_dead_resource(dst.handle);
		return false;
		FG_CATCH(false)
	}

	void handle_exception(const char *where)
	{
		if (!g.exc_disabled) logf("exception in %s - frame generation OFF for this session", where ? where : "?");
		g.exc_disabled = true;
		g.pending = false; g.r_in_bb = false;
	}

	// ReShade present event: frame k's command stream is complete (next: effects/overlay, reshade_present, real Present).
	void on_present(command_queue *, swapchain *sc, const rect *, const rect *, uint32_t, const rect *)
	{
		if (t_in_native) { ++t_foreign; return; }
		if (!g_live) return;
		FG_ENTER();
		if (sc == nullptr || sc != g.rsc) return;
		FG_TRY
		g.render_tid = GetCurrentThreadId();
		const uint64_t k = g.frame_rec;
		const double now = qpc_ms();
		g.hitch_k = false;
		if (g.last_present_ms > 0.0)
		{
			const double dt = now - g.last_present_ms;
			push_cpu(g.win.c_frame, g.last_present_ms, now);
			const double ema_dt = g.fps_ema > 0.0 ? 1000.0 / g.fps_ema : 0.0;   // average interval BEFORE this frame
			g.hitch_k = g_cfg.cut_hitch_ms > 0.0f && dt > g_cfg.cut_hitch_ms && ema_dt > 0.0 && dt > 2.5 * ema_dt;
			g.last_dt = dt;
			if (dt > 0.05 && dt < 1000.0)
			{
				const double f = 1000.0 / dt;
				g.fps_ema = g.fps_ema <= 0.0 ? f : g.fps_ema + (f - g.fps_ema) * 0.1;
			}
		}
		else g.win.start_ms = now;
		g.last_present_ms = now;

		g.fk = g.f;
		g.f = frame_flags {};
		g.ps_tag = 0;
		const bool gameplay = g.fk.main && g.fk.epost;
		if (g_cfg.enable) menu::update(gameplay);
		if (gameplay) ++g.win.gameplay;
		if (g.fk.tsaa) ++g.win.tsaa;

		if (g.rp_due && g.pending && g.pending_frame + 1 < k)
		{
			// reshade_present never ran for the previous frame and nothing presented R: drop it
			g.pending = false; g.r_in_bb = false; ++g.win.r_dropped; ++g.drop_total;
		}
		close_bracket(k);
		read_back();
		poll_cut_stats();
		// 0.4.1: presenter 2 housekeeping that never waits: a stalled pacer's exit, a finished builder, a deferred teardown
		xa::poll_stall();
		xa::poll_builder();
		if (g_cfg.xa_test_build_teardown > 0 && !xa::test_teardown_done && xa::builder && xa::builds_started == static_cast<uint64_t>(g_cfg.xa_test_build_teardown) && k >= xa::build_start_frame + 3)
		{
			// debug (XaTestTeardownInBuild): the M4 review's deadlock case - a teardown inside the ReShade present event (ReShade's
			// D3D11 lock held) while the builder runs
			xa::test_teardown_done = true;
			const double t0 = qpc_ms();
			xa::teardown(1, "TEST: teardown inside the present event while the builder runs (XaTestTeardownInBuild)");
			logf("TEST: that teardown returned in %.3f ms (builder %s)", qpc_ms() - t0, xa::builder ? "still running: teardown deferred" : "already finished");
		}
		// 0.4.0: window state for presenter 2 - minimized (detach), moved to another monitor / resized (topology again)
		if (xa::topo.hwnd)
		{
			const bool mn = IsIconic(xa::topo.hwnd) != FALSE;
			if (mn != xa_minimized) { logf("window %s", mn ? "MINIMIZED (presenter 2: the visual is detached)" : "restored"); xa_minimized = mn; }
			if (mn) ++g.win.xa_min;
			if (GetForegroundWindow() == xa::topo.hwnd) ++g.win.fore;
			if ((k % 120) == 0 && g.native_sc && g.dev && !mn && !xa::builder)
			{
				const HMONITOR mon = MonitorFromWindow(xa::topo.hwnd, MONITOR_DEFAULTTONEAREST);
				RECT rc = {}; GetClientRect(xa::topo.hwnd, &rc);
				const uint32_t cw = static_cast<uint32_t>(rc.right - rc.left), ch = static_cast<uint32_t>(rc.bottom - rc.top);
				if (mon != xa::topo.mon || cw != xa::topo.client_w || ch != xa::topo.client_h)
				{
					DXGI_SWAP_CHAIN_DESC sd = {};
					if (SUCCEEDED(g.native_sc->GetDesc(&sd)))
					{
						logf("window changed (monitor %p -> %p, client %ux%u -> %ux%u): topology re-evaluated", xa::topo.mon, mon, xa::topo.client_w, xa::topo.client_h, cw, ch);
						xa::evaluate_topology(g.native_sc, g.dev, sd.BufferDesc.Width, sd.BufferDesc.Height);
					}
				}
			}
			if ((xa::S.ok || xa::builder) && presenter_now() != 2) xa::teardown(1, "presenter 2 no longer used (topology / config)");
		}
		g.active_k = update_state(k);
		costab::tick(now, g.st == st_t::active, g.last_dt);
		// work level for frame k+1
		const int lvl = (g_cfg.profile || g.st != st_t::off) ? 2 : ((g_cfg.enable && !menu::timer_started()) ? 1 : 0);
		if (lvl != g_level)
		{
			logf("work level %d -> %d (%s)", g_level, lvl, lvl == 2 ? "full: anchors, deadline, GPU timestamps" : (lvl == 1 ? "gameplay detection only" : "inert: per-draw callbacks return at once"));
			g_level = lvl;
			if (lvl < 2) { g.bb_bound = false; g.t_pre = 0.0; }
		}
		if (g_level >= 2) open_bracket(k + 1);
		g.present_k = k;
		g.rp_due = true;
		g.frame_rec = k + 1;
		++g.win.frames;
		if ((k % 600) == 0)
		{
			scan_modules(false);   // present-hook DLLs injected after the first swap chain
			if (xa::S.ok && !xa::builder) xa::chain_hook_recheck();   // 0.4.1: a hook on the composition chain's Present
		}

		if (g_cfg.dump_key > 0 && g_cfg.enable)
		{
			const bool down = (GetAsyncKeyState(g_cfg.dump_key) & 0x8000) != 0;
			if (down && !dump::key_down) { dump::key_armed = true; logf("dump: key pressed - next generated frame is read back"); }
			dump::key_down = down;
		}
		dump::poll();

		if (g.win.frames >= static_cast<uint64_t>(g_cfg.log_window))
			log_window(now);
		FG_CATCH()
	}

	// reshade_present(k): the back buffer holds R(k) (with HUD and ReShade overlay); app state is already restored.
	void on_reshade_present(effect_runtime *rt)
	{
		if (t_in_native) { ++t_foreign; return; }
		if (!g_live) return;
		FG_ENTER();
		if (rt == nullptr || !g.rp_due || !g.imm) return;
		FG_TRY
		if (g.rt == nullptr && g.native_sc && rt->get_native() == reinterpret_cast<uint64_t>(g.native_sc))
		{
			g.rt = rt;
			logf("effect runtime %p adopted at reshade_present (runtime of our swap chain)", rt);
		}
		if (rt != g.rt) return;
		g.rp_due = false;
		const uint64_t k = g.present_k;
		if (g.native_sc && rt->get_native() != reinterpret_cast<uint64_t>(g.native_sc)) return;   // not our swap chain
		const uint64_t bb_now = rt->get_current_back_buffer().handle;
		if (bb_now != g.bb)
		{
			if (g.pending) { ++g.win.r_dropped; ++g.drop_total; }
			g.pending = false; g.r_in_bb = false; g.bb = bb_now;
			safe_release(g.bb_rtv);
		}
		ts(TS_G0);
		const double t_rp0 = qpc_ms();
		const bool eff = effective_enable();
		const bool res = eff && g.bb && ensure_resources();
		const bool fg = g.active_k && res;
		const bool mode3 = g_cfg.mode == 3;
		const int slot_cur = static_cast<int>(k & 1), slot_prev = static_cast<int>((k - 1) & 1);
		ID3D11Resource *bb = reinterpret_cast<ID3D11Resource *>(g.bb);
		pred_guard pg(res ? g.imm : nullptr);

		// 0.4.0 presenter 2: devices / surfaces / pacer are created outside ACTIVE (like the FSR context); in ACTIVE every game
		// frame becomes a job (R always, G when built) as long as the job's slot is free (CPU check, never a GPU wait)
		const bool p2 = presenter_now() == 2;
		if (p2 && res && (g.st == st_t::warmup || g.st == st_t::suspended)) xa_ensure();
		g.out = fg_t::gen_target { g.gen, g.gen_rtv, g.gen_uav };
		int xs = -1;
		uint64_t xjob = 0;
		const bool engage = p2 && fg && xa_ready() && !(g_cfg.cost_ab > 0 && costab::phase == 1);
		if (engage)
		{
			if (!xa::engaged || xa::t_last_pub <= 0.0) { xa::busy_run = 0; xa::t_last_pub = t_rp0; }   // (re)engaging: the watchdog starts now
			const uint64_t j = xa::next_job;
			if (xa::X.released.load(std::memory_order_acquire) + xa::kSlots >= j)
			{
				xs = static_cast<int>(j % xa::kSlots); xjob = j;
				if (xa::S.g_direct) g.out = fg_t::gen_target { xa::S.G[xs], xa::S.G_rtv[xs], xa::S.G_uav[xs] };
				xa::busy_run = 0;
			}
			else
			{
				++g.win.xa_busy; ++xa::busy_total;
				// 0.4.1 watchdog: the pacer consumes no jobs (stuck in a driver / DComp call, starved): the visual would keep
				// showing its last image over the game's live frames. >= 30 frames and >= 300 ms without a free slot = stalled.
				if (++xa::busy_run >= 30 && t_rp0 - xa::t_last_pub >= 300.0)
				{
					char b[120]; snprintf(b, sizeof(b), "no free slot for %u frames / %.0f ms", xa::busy_run, t_rp0 - xa::t_last_pub);
					xa::stall(b);
				}
			}
		}

		// R(k) is a usable real frame only when it passes the gameplay gate (incl. TSAA with RequireTSAA=1)
		const bool real_ok = g.fk.main && g.fk.epost && (!g_cfg.require_tsaa || g.fk.tsaa);
		if (res && real_ok)
		{
			g.imm->CopyResource(g.real[slot_cur], bb);   // save R(k) (before a late R overwrites the back buffer)
			g.real_frame[slot_cur] = k;
		}
		else g.real_frame[slot_cur] = ~0ull;

		// Mode 3 inputs. The SR export is polled on every frame while FG is not OFF (the "no DLSS SR inputs" suspend and its
		// resume depend on it); the HUD-less export - also the bridge's demand signal for its copy - from WARMUP on, so the
		// first ACTIVE frame already has one. The FSR context is created during WARMUP (its 8-16 ms create stays out of FG).
		sr_in s; hud_in hdl;   // AddRef'd textures, released at scope end
		if (mode3 && res && g.st != st_t::off)
		{
			query_sr(s);
			if (s.valid) fsr::sr_miss_run = 0; else if (fsr::sr_miss_run < 1000000) ++fsr::sr_miss_run;
			query_hud(hdl);   // 0.3.1: also while SUSPENDED (the bridge's demand keep-alive; see the 0.3.1 notes)
			if (g.st == st_t::warmup || g.st == st_t::suspended) fsr_ensure();   // 0.3.1: never created inside ACTIVE
		}

		// CPU cut signals for R(k-1) -> R(k): a frame hitch (load/respawn stall) or a main-pass size change
		const char *cut = nullptr;
		if (g.hitch_k) cut = "frame hitch";
		else if (g.hitch_next == k) cut = "frame after a hitch (the long simulation step lands here)";
		else if (g.fk.main_w && g.prev_main_w && (g.fk.main_w != g.prev_main_w || g.fk.main_h != g.prev_main_h)) cut = "main-pass size change";
		if (g.fk.main_w) { g.prev_main_w = g.fk.main_w; g.prev_main_h = g.fk.main_h; }
		if (g.hitch_k) g.hitch_next = k + 1;   // 0.3.1: m3b: the GPU guard fired at k+1 after 8 of 10 hitches at k

		bool late = false;
		if (g.pending && p2) { g.pending = false; g.r_in_bb = false; ++g.win.r_dropped; ++g.drop_total; }   // presenter switched 1 -> 2
		if (g.pending && !p2)
		{
			g.r_in_bb = false;   // the game has written the back buffer since: copy R again
			if (fg && pending_valid()) { inject_r(A_LATE); late = true; }   // R(k-1) never found an anchor: present it now, G follows
			else { g.pending = false; ++g.win.r_dropped; ++g.drop_total; }
		}

		const bool pair = fg && g.real_frame[slot_prev] == k - 1 && g.real_frame[slot_cur] == k && !g.occluded;
		bool built = false;
		int m = g_cfg.mode >= 2 ? 2 : 1;
		if (mode3)
		{
			// FSR: dispatch on every FG frame with a saved R(k) (FSR keeps R(k-1) itself); a reset dispatch shows no G
			const bool cur_ok = fg && g.real_frame[slot_cur] == k && !g.occluded;
			if (cur_ok && cut)
			{
				++g.win.cut_cpu; ++g.cut_cpu_total;
				if (g.cut_cpu_total <= 40) logf("cut: frame %llu (%s, frame interval %.1f ms, fps %.1f) - FSR history reset, no generated frame", k, cut, g.last_dt, g.fps_ema);
			}
			if (cur_ok) built = fsr_frame(k, slot_prev, slot_cur, pair, cut, s, hdl, m);
			else fsr::need_reset = true;
		}
		else if (pair && cut)
		{
			// camera cut: no G this frame (a blend of unrelated frames = double exposure); the game's present shows R(k)
			++g.win.cut_cpu; ++g.cut_cpu_total;
			if (g.cut_cpu_total <= 20) logf("cut: frame %llu (%s, frame interval %.1f ms, fps %.1f) - no generated frame, R(k) is shown by the game's present", k, cut, g.last_dt, g.fps_ema);
		}
		else if (pair)
		{
			if (m == 2) built = build_blend(slot_prev, slot_cur);
			if (!built) { m = 1; g.imm->CopyResource(g.out.tex, g.real[slot_prev]); }
			built = true;
		}

		if (p2)
		{
			// presenter 2: no native Present, the back buffer is never replaced (the game's own present shows R(k) under the
			// visual, which is also exactly what appears when the visual is detached)
			const uint32_t seq_g = static_cast<uint32_t>((2 * k) & 4095), seq_r = static_cast<uint32_t>((2 * k + 1) & 4095);
			if (built && dump::due(g.g_total + 1)) dump::queue(slot_prev, slot_cur, k, m, (mode3 && hdl.valid) ? hdl.io.texture : nullptr, g.g_total + 1);
			if (built && xs >= 0)
			{
				if ((g_cfg.marker & 1) && g.imm1 && g.out.rtv)
				{
					const float magenta[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
					const D3D11_RECT r = { 0, 0, 48, 48 };
					g.imm1->ClearView(g.out.rtv, magenta, &r, 1);
				}
				stamp_barcode(g.out.rtv, false, k);
				if (!xa::S.g_direct) g.imm->CopyResource(xa::S.G[xs], g.gen);   // (no UAV on shared textures: FSR wrote gen)
			}
			else if (built) ++g.win.xa_gdrop;                                   // slot busy: this G is not shown
			if ((g_cfg.marker & 2) && g.bb && g.dev && (res || ensure_resources())) stamp_barcode(bb_rtv(), true, k);   // R(k) carries its barcode on screen, FG on or off
			if (xs >= 0)
			{
				g.imm->CopyResource(xa::S.R[xs], bb);   // R(k) incl. HUD, ReShade overlay and the barcode
				ts(TS_G);
				xa::publish(xjob, k, built, seq_g, seq_r);
				++xa::next_job;
				xa::engaged = true;
				xa::t_last_pub = t_rp0;
				++g.win.xa_jobs; if (built) { ++g.win.xa_jobs_g; ++g.win.fg_frames; ++g.win.g_built; ++g.g_total; }
			}
			else
			{
				ts(TS_G);
				if (xa::engaged && !engage)
				{
					// stop issuing jobs: the pacer detaches the visual once the last issued job is on screen
					xa::X.detach_req.store(xa::X.pub.load(std::memory_order_relaxed), std::memory_order_release);
					if (xa::ev_ctl) SetEvent(xa::ev_ctl);
					xa::engaged = false;
					++g.win.xa_detreq;
				}
			}
		}
		else if (built)
		{
			// dump before any marker (gen as the viewer gets it, without the test marks)
			if (dump::due(g.g_total + 1)) dump::queue(slot_prev, slot_cur, k, m, (mode3 && hdl.valid) ? hdl.io.texture : nullptr, g.g_total + 1);
			if ((g_cfg.marker & 1) && g.imm1 && g.gen_rtv)
			{
				const float magenta[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
				const D3D11_RECT r = { 0, 0, 48, 48 };
				g.imm1->ClearView(g.gen_rtv, magenta, &r, 1);
			}
			stamp_barcode(g.gen_rtv, false, k);
			g.imm->CopyResource(bb, g.gen);
			ts(TS_G);
			if (rec_q *r = open_rec()) r->g_active = true;
			g.pending = true; g.r_in_bb = false; g.pending_slot = slot_cur; g.pending_frame = k; g.pending_gen = g.sc_gen;
			++g.win.fg_frames; ++g.win.g_built; ++g.g_total;
		}
		else
		{
			// no G: the game's own present shows R(k). After a late R(k-1) the back buffer holds R(k-1): put R(k) back.
			bool bb_is_rk = !late;
			if (late && res && g.real_frame[slot_cur] == k) { g.imm->CopyResource(bb, g.real[slot_cur]); bb_is_rk = true; }
			if (bb_is_rk && (g_cfg.marker & 2) && g.bb && g.dev && ensure_resources()) stamp_barcode(bb_rtv(), true, k);
			ts(TS_G);
		}
		g.t_rp_end = qpc_ms();
		if (g.st != st_t::off) push_cpu(g.win.c_rp, t_rp0, g.t_rp_end);
		if (g_cfg.present_log && (g_cfg.marker & 2)) fcsv::row(static_cast<uint32_t>((2 * k + 1) & 4095), k, g.t_rp_end, xjob, static_cast<int>(g.st));
		g.out = fg_t::gen_target { g.gen, g.gen_rtv, g.gen_uav };   // 0.4.1: never leave a pointer to a presenter-2 slot behind
		FG_CATCH()
	}
	void on_finish_present(command_queue *, swapchain *sc)
	{
		if (t_in_native) { ++t_foreign; return; }
		if (!g_live) return;
		FG_ENTER();
		if (sc == nullptr || sc != g.rsc) return;
		FG_TRY
		ts(TS_FP);
		const double now = qpc_ms();
		if (g.t_rp_end > 0.0) { push_cpu(g.win.c_proxy, g.t_rp_end, now); g.t_rp_end = 0.0; }
		g.t_frame_start = now;
		g.t_pre = 0.0;
		FG_CATCH()
	}
}

// For other add-ons (tf2-dlss-sr, tf2-nr-bridge): true while tf2-fg's own native IDXGISwapChain::Present runs on the
// calling thread. Events raised then come from a third-party present hook rendering through ReShade's proxy device, in
// the middle of a game frame; a callback should return at once. Resolve with GetModuleHandleW(L"tf2-fg.addon64") +
// GetProcAddress(..., "tf2fg_in_native_present").
extern "C" __declspec(dllexport) bool tf2fg_in_native_present()
{
	return t_in_native;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
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
		g_log = _wfsopen((g_dir / "tf2-fg.log").c_str(), L"w", _SH_DENYNO);
		logf("tf2-fg %s loaded (frame generation: Mode 3 FSR 3 frame interpolation, Mode 2 linear blend, Mode 1 duplicate+marker; presenter: 2 = xa + DComp from the display adapter, 1 = GPU-timeline present injection)", kVersion);
		load_config();
		xa_sync_cfg();
		g_live = g_cfg.enable != 0 || g_cfg.profile != 0;
		g_level = g_cfg.profile ? 2 : (g_cfg.enable ? 1 : 0);
		if (!g_live) logf("Enable=0 and Profile=0: inert (only device/swap-chain bookkeeping; no per-draw work, no timestamps)");
		reshade::register_event<reshade::addon_event::create_device>(on_create_device);
		reshade::register_event<reshade::addon_event::init_device>(on_init_device);
		reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
		reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
		reshade::register_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
		reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
		reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
		reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
		reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
		reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_rts);
		reshade::register_event<reshade::addon_event::draw>(on_draw);
		reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_indirect);
		reshade::register_event<reshade::addon_event::dispatch>(on_dispatch);
		reshade::register_event<reshade::addon_event::clear_render_target_view>(on_clear_rt);
		reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(on_clear_ds);
		reshade::register_event<reshade::addon_event::copy_resource>(on_copy_resource);
		reshade::register_event<reshade::addon_event::copy_texture_region>(on_copy_texture_region);
		reshade::register_event<reshade::addon_event::resolve_texture_region>(on_resolve);
		reshade::register_event<reshade::addon_event::copy_buffer_to_texture>(on_copy_buffer_to_texture);
		reshade::register_event<reshade::addon_event::update_texture_region>(on_update_texture);
		reshade::register_event<reshade::addon_event::present>(on_present);
		reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
		reshade::register_event<reshade::addon_event::finish_present>(on_finish_present);
		break;
	}
	case DLL_PROCESS_DETACH:
		reshade::unregister_addon(module);
		if (reserved != nullptr)
		{
			// 0.4.1: process termination - every other thread (pacer, builder, render) is already gone, possibly while it owned
			// g_log_mx or a CRT FILE lock: taking either could hang the exit. The OS closes the handles.
			g_log = nullptr;
			break;
		}
		{
			std::lock_guard lock(g_log_mx);   // a concurrent logf must never write to a closed FILE*
			FILE *f = g_log;
			g_log = nullptr;
			if (f) fclose(f);
		}
		if (fcsv::f) { fcsv::flush(); fclose(fcsv::f); fcsv::f = nullptr; }
		break;
	}
	return TRUE;
}
