// tf2_fg_interop.h - C interface between tf2-dlss-sr / tf2-nr-bridge (exporters) and tf2-fg (frame generation).
//
// Header-only, plain C layout, no dependencies beyond <stdint.h> and a forward-declared ID3D11Texture2D. Include it
// from tf2-fg as "../bridge/tf2_fg_interop.h". Version 1 (tf2-dlss-sr 0.3.0, tf2-nr-bridge 0.4.0, 2026-09-29).
// v1 additions (same struct layout; new flag bits only, older consumers ignore them): TF2SR_FG_CAMERA_CUT (tf2-dlss-sr
// 0.3.1); the bridge invalidates at a missed capture point (tf2-nr-bridge 0.4.1).
//
// Resolving (from tf2-fg, at run time; never link against the exporters):
//     HMODULE sr = GetModuleHandleW(L"tf2-dlss-sr.addon64");
//     auto get_inputs = sr ? (PFN_tf2sr_get_fg_inputs)GetProcAddress(sr, "tf2sr_get_fg_inputs") : nullptr;
//     HMODULE br = GetModuleHandleW(L"tf2-nr-bridge.addon64");
//     auto get_hudless = br ? (PFN_tf2nr_get_hudless)GetProcAddress(br, "tf2nr_get_hudless") : nullptr;
//   Re-resolve after a device re-creation (ReShade unloads/reloads add-ons with the last device). Either module may be
//   missing (the export then simply does not exist: run FG without it / reset).
//
// Common rules (both exports):
//   * Call with struct_size = sizeof(the v1 struct). A smaller struct_size is refused (return -1, nothing written).
//     Later versions only APPEND fields; `version` says which fields were filled; `struct_size` returns the bytes written.
//   * Returned texture pointers are NATIVE ID3D11Texture2D* of the game's device (not ReShade proxies), AddRef'd for the
//     caller: Release() each non-null one when done. They are returned only when the call returns 1 (VALID); otherwise
//     every texture pointer is null and nothing needs releasing.
//   * Textures are never destroyed while a caller holds a reference, but their CONTENTS are rewritten by the exporter
//     every game frame on the immediate context. Consume them only in GPU work queued between the capture point of
//     `game_frame` and the exporter's next capture point (the next TSAA draw for SR, the next pre-HUD point for the
//     bridge). tf2-fg's reshade_present / finish_present of the same game frame is inside that window.
//   * `generation` changes whenever the exporter (re)allocates the textures (size change, device loss). Treat a changed
//     generation like a camera cut (reset FSR history) and drop cached views of the old textures.
//   * Frame numbering: each exporter counts ReShade `present` events (one per game frame, +1 at the END of its own
//     present callback). `game_frame` is that counter while the data was produced; `exporter_frame` is the counter now.
//     At tf2-fg's reshade_present/finish_present of game frame F (all add-ons' present events have run) fresh data
//     has exporter_frame == game_frame + 1 for BOTH exporters. Inside frame F's command stream after the capture point
//     it has exporter_frame == game_frame. The two exporters' counters are not guaranteed equal to each other or to
//     tf2-fg's: compare each with its own exporter_frame, never across add-ons.
//   * VALID means: the exporter produced the data in game_frame, game_frame is the frame being rendered or the frame
//     whose present has just been processed (exporter_frame - game_frame <= 1), and no later capture point was passed
//     without producing data. A capture point that produces nothing invalidates right there: SR at a TSAA draw without a
//     DLSS evaluate, the bridge at an engine_post draw into the back buffer without a HUD-less copy (0.4.1; 0.4.0 only
//     at present); a frame without any capture point (menus, loading) invalidates at the exporter's present. So between
//     present(F) and frame F+1's capture point, frame F's data is still returned VALID with exporter_frame ==
//     game_frame + 1 - by design: that is exactly the window in which tf2-fg consumes it (reshade_present(F)).
//   * Thread safety: callable from any thread (short internal lock, no D3D calls, no waits). Crash safety: no
//     exceptions escape (-2 is returned instead); the struct pointer itself is not validated beyond null.
//   * Both exporters also skip all their per-draw work while tf2fg_in_native_present() (tf2-fg.addon64) returns true.
#pragma once
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
struct ID3D11Texture2D;
#else
typedef struct ID3D11Texture2D ID3D11Texture2D;
#endif

#define TF2_FG_INTEROP_VERSION 1u

// =====================================================================================================================
// tf2-dlss-sr: per-frame DLSS SR inputs (the prep_cs outputs DLSS itself consumed this frame).
//   int tf2sr_get_fg_inputs(tf2sr_fg_inputs_v1 *io)  ->  1 VALID (textures AddRef'd), 0 not valid, -1 bad argument,
//                                                        -2 internal exception
// =====================================================================================================================
#define TF2SR_FG_INPUTS_VERSION 1u

#define TF2SR_FG_VALID            0x001u  // see "VALID" above; depth/motion_vectors/exposure returned (AddRef'd)
#define TF2SR_FG_RESET            0x002u  // DLSS history reset in game_frame: camera cut (TSAA blend >= 0.999), a gap of
                                          // > 2 frames since the previous TSAA frame, a new/re-created DLSS feature, or a
                                          // runtime jitter/MV-convention change. FSR: reset = true.
#define TF2SR_FG_MV_JITTER_FREE   0x004u  // motion vectors have the TSAA jitter removed ([TF2DLSSSR] MVJittered=1, default)
#define TF2SR_FG_DEPTH_INVERTED   0x008u  // depth is reversed (1 near, 0 far). Never set today ([TF2DLSSSR] DepthInverted=0)
#define TF2SR_FG_UPSCALING        0x010u  // render size < display size (SR modes 2..5); clear for DLAA (mode 1)
#define TF2SR_FG_EXPOSURE_FORCED  0x020u  // exposure_value = TF2 c_forceExposure (else the exposure lives only on the GPU)
#define TF2SR_FG_CAMERA_VALID     0x040u  // camera_near/far/fov_y/aspect = game_frame's main view (see the camera fields)
#define TF2SR_FG_CAMERA_FAR_INF   0x080u  // projection has an infinite (or > 1e7 units) far plane; camera_far = 1e7
#define TF2SR_FG_SIZE_FROM_TSAA   0x100u  // render size came from TSAA's own clamp (CBufTSAA c_fbMaxUVs) and disagreed
                                          // with the main-pass viewport (e.g. the death cam) - informational
#define TF2SR_FG_CAMERA_CUT       0x200u  // (0.3.1) game_frame's main-view camera jumped against game_frame - 1's: moved
                                          // more than [TF2DLSSSR] CamCutDist world units (64), turned more than CamCutDeg
                                          // (40 deg) or changed fovY by more than CamCutFovDeg (15 deg) in one frame
                                          // (death cam, kill replay, respawn, spectator switches). DLSS is NOT reset by it
                                          // (RESET stays TSAA's own signal). FSR: reset = true.

typedef struct tf2sr_fg_inputs_v1
{
	uint32_t struct_size;         // IN sizeof(tf2sr_fg_inputs_v1); OUT bytes written
	uint32_t version;             // OUT TF2SR_FG_INPUTS_VERSION
	uint32_t flags;               // TF2SR_FG_*
	int32_t  sr_mode;             // 0 off, 1 DLAA, 2 Quality, 3 Balanced, 4 Performance, 5 Ultra Performance (of game_frame)
	uint64_t game_frame;          // tf2-dlss-sr frame counter during the frame that produced the data
	uint64_t exporter_frame;      // tf2-dlss-sr frame counter now
	uint64_t eval_serial;         // successful DLSS evaluates since load (+1 per evaluated frame; equal = same data)
	uint64_t generation;          // texture generation (see common rules)

	// R32_FLOAT, exactly render_width x render_height (the whole texture is the render rect, top-left origin, no
	// padding), SRV + UAV bind flags, 1 mip. TF2's main depth-stencil (R24 UNORM) converted to float: device Z,
	// standard (0 = near plane, 1 = far plane, not linear). The first-person weapon is drawn with a compressed depth
	// range (viewport MaxDepth < 1), so its pixels have small depth values (always "near").
	ID3D11Texture2D *depth;
	// R16G16_FLOAT, render_width x render_height, SRV + UAV, 1 mip. Per-pixel motion in RENDER PIXELS pointing from the
	// current pixel to where that surface was in the previous frame: prev_pos = cur_pos + mv (x right, y down, i.e.
	// texture orientation), camera + object motion from TF2's own velocity target, jitter-free when
	// TF2SR_FG_MV_JITTER_FREE. Same texture DLSS got with InMVScale 1,1 (MVLowRes). FSR FI: motionVectorScale (1, 1)
	// with the LOW_RES_MV permutation (FI divides by the render size itself) - the tf2fg_fsr.h convention.
	ID3D11Texture2D *motion_vectors;
	// R16_FLOAT 1x1, SRV: the exposure multiplier TF2 applies before its tonemap (TSAA's t30, or c_forceExposure), as
	// given to DLSS pInExposureTexture. Linear scene * exposure ~ "1.0 = normally exposed".
	ID3D11Texture2D *exposure;

	uint32_t render_width, render_height;    // sizes of depth / motion_vectors = DLSS render sub-rect of game_frame
	uint32_t display_width, display_height; // TSAA/DLSS output size (= back-buffer size, 1920x1200 on this laptop)
	// Projection jitter of game_frame in render pixels, exactly the DLSS InJitterOffsetX/Y values:
	// jitter_px = JitterSign * c_jitter * render_size with [TF2DLSSSR] JitterSignX/Y = -1 (default, verified with DLSS):
	// jitter_x_px = -c_jitterX * render_width, jitter_y_px = -c_jitterY * render_height (+x right, +y down).
	float jitter_x_px, jitter_y_px;
	float jitter_x_uv, jitter_y_uv;         // raw CBufTSAA c_jitterX / c_jitterY (render-viewport UV units, y down)
	float exposure_value;                   // TF2 c_forceExposure when TF2SR_FG_EXPOSURE_FORCED, else 0
	float mv_scale_x, mv_scale_y;           // always 1, 1 (MVs are already in render pixels)
	// Camera of game_frame's main view: TF2's CBufCommonPerCamera (b2) upload current at the first draw of the main scene
	// pass, decoded per research\fg_hdr\camera_params.md sec. 4 (near = -C[2][3]/k, tanHalfY from the matrix rows with
	// the jitter term removed). Only set (CAMERA_VALID) when that snapshot is from game_frame, is a perspective view with
	// viewport MaxZ 1, c_zNear agrees with the matrix, and c_renderTargetSize * c_viewportScale == render size +-2 px.
	// TF2's projection is infinite (k == 1): CAMERA_FAR_INF + camera_far = 1e7 (FSR: DEPTH_INFINITE or far 1e7).
	// Units are Source/TF2 world units (inches): tf2fg_fsr CreateOptions::viewSpaceToMeters = 0.0254.
	float camera_near, camera_far;          // > 0 when TF2SR_FG_CAMERA_VALID
	float camera_fov_y;                     // vertical field of view in radians (0, pi) when TF2SR_FG_CAMERA_VALID
	float camera_aspect;                    // x/y projection scale ratio (= render aspect for square pixels)
	uint64_t camera_frame;                  // = game_frame when CAMERA_VALID, else 0
	uint32_t tsaa_render_width, tsaa_render_height;   // TSAA's own sub-rect (c_fbMaxUVs) in game_frame (diagnostic)
} tf2sr_fg_inputs_v1;

typedef int (*PFN_tf2sr_get_fg_inputs)(tf2sr_fg_inputs_v1 *io);

// =====================================================================================================================
// tf2-nr-bridge: HUD-less colour of the final frame, copied right before TF2 draws the HUD.
//   int tf2nr_get_hudless(tf2nr_hudless_v1 *io)  ->  1 VALID (texture AddRef'd), 0 not valid, -1 bad argument,
//                                                    -2 internal exception
//   Calling it is also the DEMAND signal: the bridge makes the copy (and, with DLSS 5 NR off in the Video menu, runs its
//   pre-HUD effect cut so ReShade effects are not applied on top of the HUD) only while the export has been called
//   within the last 120 game frames, or always with [TF2NRBridge] HudlessCopy=2 (0 = never, 1 = on demand, default).
// =====================================================================================================================
#define TF2NR_HUDLESS_VERSION 1u

#define TF2NR_HUDLESS_VALID          0x001u  // texture holds game_frame's HUD-less image (see "VALID" above)
#define TF2NR_HUDLESS_LDR_CUT        0x002u  // captured after the bridge's LDR cut (engine_post done, ReShade effects run)
#define TF2NR_HUDLESS_HDR_CUT        0x004u  // captured after engine_post in HdrCut mode (effects ran on the HDR scene)
#define TF2NR_HUDLESS_NR_ON          0x008u  // DLSS 5 NR was on (menu) in game_frame
#define TF2NR_HUDLESS_EFFECTS_PREHUD 0x010u  // ReShade effects of this frame ran before the copy (none run at present)
#define TF2NR_HUDLESS_CUT_FOR_FG     0x020u  // the cut ran only because of FG demand (NR off in the menu)

typedef struct tf2nr_hudless_v1
{
	uint32_t struct_size;         // IN sizeof(tf2nr_hudless_v1); OUT bytes written
	uint32_t version;             // OUT TF2NR_HUDLESS_VERSION
	uint32_t flags;               // TF2NR_HUDLESS_*
	uint32_t dxgi_format;         // format of `texture`: DXGI_FORMAT_R8G8B8A8_TYPELESS (27)
	uint32_t back_buffer_format;  // DXGI format of the back buffer it was copied from (29 = R8G8B8A8_UNORM_SRGB today)
	uint32_t width, height;       // = back-buffer size (1920x1200)
	uint32_t ring_index;          // which of the bridge's 2 textures (alternates per captured frame)
	uint64_t game_frame;          // tf2-nr-bridge frame counter during the captured frame
	uint64_t exporter_frame;      // tf2-nr-bridge frame counter now
	uint64_t capture_serial;      // captures since load (+1 per captured frame)
	uint64_t generation;          // texture generation (see common rules)
	// R8G8B8A8_TYPELESS, width x height, SRV bind flag, 1 mip, 1 sample. A CopyResource of the back buffer right after
	// engine_post (tonemapped, graded, sRGB-encoded 8-bit, the bridge's pre-HUD ReShade effects applied) and before
	// the first HUD/RUI draw. Bit-identical to the presented back buffer wherever no HUD/overlay was drawn. Create an
	// R8G8B8A8_UNORM view to read the encoded bytes (FSR Transfer::Srgb) or _UNORM_SRGB to read linear values.
	// The ring texture of game_frame is not written again before game_frame + 2's capture (two-texture ring).
	ID3D11Texture2D *texture;
} tf2nr_hudless_v1;

typedef int (*PFN_tf2nr_get_hudless)(tf2nr_hudless_v1 *io);

// tf2-fg.addon64 (existing): true while tf2-fg's own native IDXGISwapChain::Present runs on the calling thread.
typedef bool (*PFN_tf2fg_in_native_present)();
