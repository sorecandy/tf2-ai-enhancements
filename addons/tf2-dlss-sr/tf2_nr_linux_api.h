// tf2_nr_linux_api.h - C interface of tf2-nr-linux.addon64 (DLSS 5 Neural Rendering for Titanfall 2 on Linux).
// This header is the only part tf2-dlss-sr (MIT) uses; it may be used under the MIT licence. The add-on itself is
// GPL-3.0-or-later (its NGX runtime and D3D12 interop come from DynamicShaderFrameGen).
//
// tf2-dlss-sr resolves the exports with GetModuleHandleW(L"tf2-nr-linux.addon64") + GetProcAddress and calls them on
// the render thread with the game's native ID3D11Device / immediate context.
#pragma once
#include <stdint.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

#define TF2NR_LINUX_FRAME_VERSION 1u

struct tf2nr_linux_frame
{
	uint32_t version;              // TF2NR_LINUX_FRAME_VERSION
	ID3D11Device *device;
	ID3D11DeviceContext *context;  // immediate context
	ID3D11Texture2D *color;        // in/out: R16G16B16A16_FLOAT, SRV + UAV, linear HDR before tonemapping, alpha kept
	ID3D11Texture2D *depth;        // R32_FLOAT, device Z (0 near), same size as color
	ID3D11Texture2D *motion;       // R16G16_FLOAT, pixels, current + mv = previous, same size as color
	ID3D11Texture2D *exposure;     // 1x1 R16_FLOAT multiplier before the filmic curve, or null
	uint32_t width, height;        // extent used in all three textures (top-left)
	float mv_scale_x, mv_scale_y;  // multiplies the motion vectors into pixels of this extent
	uint32_t reset;                // 1 = history invalid (camera cut, new size)
};

// 1 = NR ran and color holds the result; 0 = not applied (disabled, failed, latched) - color is unchanged.
typedef int (*PFN_tf2nr_linux_apply)(const tf2nr_linux_frame *frame);
// Releases every D3D11/D3D12 object (call before the game's device goes away).
typedef void (*PFN_tf2nr_linux_reset)(void);
