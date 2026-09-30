// tf2fg_fsr.h - FidelityFX Frame Interpolation 1.1 + Optical Flow 1.1.1 on a native D3D11 device.
//
// Small C++ wrapper over the patched FidelityFX SDK DX11 backend (sdk_patched/). Static /MT Release library
// (lib/build/tf2fg_fsr.lib, built by lib/build_lib.bat), meant to be linked into the presenter ReShade add-on (M3).
//
// Linking: add lib/build/tf2fg_fsr.lib to the add-on link line (cl /std:c++17 /O2 /MT). The tf2fg:: functions are
// C++ and never throw, so the add-on's own /EHsc is fine; the library itself is built with /EHs (no 'c') because the
// FFX C entry points it calls throw on D3D11 failures (do not call ffx* functions directly from /EHsc code). No extra
// system libs are needed (D3D11 is only used through the device's COM interfaces). The lib also defines global C
// symbols of the FFX SDK (ffx*, *DX11): do not link a second FidelityFX SDK into the same module.
// lib/build_testing/tf2fg_fsr_testing.lib is the test flavour (TF2FG_TESTING: counters, fault injection, hooks);
// never link it into the add-on.
//
// Device: pass the NATIVE device (reshade::api::device::get_native()), never ReShade's proxy. ReShade hooks
// ID3D11Texture2D::GetDevice to return the proxy, so a device obtained from a texture is the wrong one; create()
// refuses the proxy (it answers QueryInterface for ReShade's D3D11Device UUID) with InvalidArgument.
//
// Requirements: the device must be FL 11_1 (checkDeviceSupport() says why not). Today's game device is FL 11_0, so
// M3 has to request 11_1 at device creation (ReShade create_device) or FI stays disabled (create() -> Unsupported).
//
// Per frame (present thread): prepare() once the frame's depth + motion vectors exist (SR prep outputs), then
// dispatch() with the back buffer (+ HUD-less copy) to produce the frame half-way between the previous dispatch's
// colour and this one. dispatch() without a successful prepare() for the same frame is refused. A prepare() result
// is consumed by the next dispatch() call whatever its outcome, and every refused/skipped call makes the next
// successful dispatch() a reset (output = current frame). dispatch() also resets FI + OF by itself when frameID is
// not the previous dispatch's frameID + 1 or when HUD-less presence changes (menus pass no HUD-less copy).
//
// Threading / context: every call records on the device's IMMEDIATE context and must be made from the thread that
// owns it (the present thread). Nothing here is thread safe. The log callback runs synchronously inside the calls:
// it must not call back into tf2fg or touch the immediate context. Re-entrant prepare()/dispatch() return
// InvalidArgument, a re-entrant destroy() is ignored (logged), lastGpuTimeMs() returns false.
//
// Stack: FFX, the D3D11 runtime and the driver run on the caller's thread. prepare()/dispatch() refuse (InvalidArgument,
// context stays usable) when less than CreateOptions::minStackHeadroomKB is left, and log the headroom once.
//
// State: all immediate-context state is preserved. Default mode swaps to a private ID3DDeviceContextState
// (created at the device's own feature level) for the duration of the call and swaps back, so the caller's
// pipeline state, bindings and hazard state are exactly what they were. Fallback mode (if the private state cannot
// be created, or a swap does not hand back the caller's state) saves/restores the compute-shader stage manually.
//
// Crash safety (Northstar vectored exception handler): every entry point validates its inputs, never divides by a
// caller-controlled value, catches every C++ exception (the backend reports D3D failures by throwing), and after
// ANY failure the context enters a failed state in which prepare()/dispatch() refuse to run. Call destroy() and
// create() again to recover.
#pragma once

#include <stdint.h>
#include <d3d11.h>

namespace tf2fg {

enum class Status : int32_t {
    Ok = 0,
    InvalidArgument,   // bad pointer / size / format / sequence (e.g. dispatch without prepare)
    Unsupported,       // device lacks a required capability (feature level, typed UAV loads, ...)
    NotInitialized,    // null context, or the context is in the failed state after an earlier error
    FfxError,          // an FFX call returned an error code
    Exception,         // a C++ exception was caught at the API boundary
    DeviceError,       // a D3D11 call failed (resource / view / state creation)
    Failed,            // anything else
};

const char* statusName(Status s);

enum class LogLevel : int32_t { Info = 0, Warn = 1, Error = 2 };
typedef void (*LogFn)(void* user, LogLevel level, const char* message);

// How immediate-context state is preserved around the FFX work.
enum class StateMode : int32_t {
    SwapContextState = 0,  // private ID3DDeviceContextState + SwapDeviceContextState (default, exact)
    ManualComputeSave = 1, // save/restore CS shader, CBs, SRVs, UAVs, samplers only
};

enum class Transfer : int32_t {
    Srgb = 0,              // 8-bit sRGB-encoded colour (the only mode supported for RGBA8 inputs)
};

struct CreateOptions {
    LogFn     log = nullptr;          // optional log sink (called synchronously, never after destroy())
    void*     logUser = nullptr;
    StateMode stateMode = StateMode::SwapContextState;
    bool      gpuTiming = false;      // timestamp queries around OF+FI in dispatch(); read with lastGpuTimeMs()
    // FI viewSpaceToMetersFactor = metres per unit of PrepareParams::cameraNear/cameraFar (view depth is derived
    // from them). Default 0.0254 = Source engine units (inches). Use 1.0 when near/far are in metres. Must be
    // finite and > 0 (create() returns InvalidArgument otherwise).
    float     viewSpaceToMeters = 0.0254f;
    uint32_t  minStackHeadroomKB = 256; // prepare()/dispatch() refuse with less thread stack left (64..65536)
    bool      debugTearLines = false; // FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_TEAR_LINES
    bool      debugView = false;      // FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_VIEW
};

// Per-frame game data, recorded as soon as depth + motion vectors of the frame exist.
//   depth         : Texture2D R32_FLOAT (or R32_TYPELESS), exactly renderWidth x renderHeight, SRV bindable,
//                   standard Z (near 0, far 1).
//   motionVectors : Texture2D R16G16_FLOAT (or R16G16_TYPELESS), exactly renderWidth x renderHeight, SRV bindable,
//                   in RENDER PIXELS with cur + mv = prev (the SR prep convention, prep_cs.hlsl u1).
//   jitter        : camera jitter in render pixels (passed through; the built permutation does not use it).
struct PrepareParams {
    ID3D11Texture2D* depth = nullptr;
    ID3D11Texture2D* motionVectors = nullptr;
    float    jitterX = 0.0f, jitterY = 0.0f;
    uint32_t renderWidth = 0, renderHeight = 0;
    float    cameraNear = 0.0f, cameraFar = 0.0f;  // > 0, different (either order); unit: see viewSpaceToMeters
    float    cameraFovY = 0.0f;                    // radians, (0, pi)
    float    frameTimeDeltaMs = 0.0f;              // >= 0
    uint64_t frameID = 0;                          // +1 per frame; anything else resets FI + OF history
    bool     reset = false;                        // camera cut
};

// Generates the frame half-way between the previous dispatch's colour and currentBackBuffer.
//   currentBackBuffer : Texture2D RGBA8 family (R8G8B8A8_UNORM / _UNORM_SRGB / _TYPELESS), display size, 1 sample.
//   hudless           : optional, same format family and size, the same frame without HUD. When set, FI takes the
//                       HUD from currentBackBuffer and does not interpolate it.
//   output            : Texture2D RGBA8 family, display size, 1 sample. Receives the interpolated frame (same
//                       encoding as the inputs). May not alias hudless.
struct DispatchParams {
    ID3D11Texture2D* currentBackBuffer = nullptr;
    ID3D11Texture2D* hudless = nullptr;
    ID3D11Texture2D* output = nullptr;
    Transfer transfer = Transfer::Srgb;
    bool     reset = false;
};

struct Context;  // opaque

// Returns Ok when the device can run OF + FI, else Unsupported with a human readable reason.
//   FL >= 11_1 (13 UAV slots in the SPD pyramid passes), ID3D11Device1/ID3D11DeviceContext1,
//   D3D11_FEATURE_DATA_D3D11_OPTIONS2::TypedUAVLoadAdditionalFormats, UAV typed load for R16G16_SINT, R8_UINT,
//   R8G8B8A8_UNORM, R16G16B16A16_FLOAT, typed UAV store for every format FI/OF write.
Status checkDeviceSupport(ID3D11Device* device, char* reasonOut, uint32_t reasonSize);

// maxRender = the largest render size prepare() will ever be given (dynamic resolution below it is fine).
Status create(ID3D11Device* nativeDevice, uint32_t displayWidth, uint32_t displayHeight,
              uint32_t maxRenderWidth, uint32_t maxRenderHeight, const CreateOptions* options, Context** outContext);

Status prepare(Context* context, const PrepareParams& params);
Status dispatch(Context* context, const DispatchParams& params);

// Releases every D3D object and FFX context. Safe on nullptr and on failed contexts.
void destroy(Context* context);

// true + the most recent resolved OF+FI GPU time (ms) when CreateOptions::gpuTiming was set.
bool lastGpuTimeMs(Context* context, double* milliseconds);

// true after a failure; prepare/dispatch refuse until the context is recreated.
bool isFailed(const Context* context);

// Debug switch (both flavours; added for tf2-fg 0.4's in-game MV/depth A/B, review finding F7): false = FI gets
// opticalFlowScale 0, binds no optical-flow resources and interpolates from the game motion vectors (+ depth) alone;
// the OF pass still runs, its result is unused. Default true. Takes effect at the next dispatch(); callers should reset.
void setOpticalFlowEnabled(Context* context, bool enabled);

#if defined(TF2FG_TESTING)
// ---- test instrumentation: only in lib/build_testing/tf2fg_fsr_testing.lib (built with /DTF2FG_TESTING) ----
// D3D11 object creations (views, textures, buffers) / constant-buffer Map calls the FFX backend has checked so far.
unsigned long long testingBackendCreations();
unsigned long long testingBackendMaps();
// Makes the n-th next backend creation / Map fail with E_OUTOFMEMORY (TIF then throws inside FFX; a failing Map
// inside fpExecuteGpuJobs is caught there and returned as an FFX error). 0 = off.
void testingFailBackendCreation(unsigned long long n);
void testingFailBackendMap(unsigned long long n);
// Non-null resource registrations so far / make the n-th next one RETURN FFX_ERROR_OUT_OF_RANGE (no throw), the
// path a registration-slot exhaustion takes. 0 = off.
unsigned long long testingBackendRegistrations();
void testingFailBackendRegistration(unsigned long long n);
bool testingFaultPending();
// Called for every SRV/UAV the backend binds as NULL (pass = shader debug name, binding = reflection name).
typedef void (*TestingNullBindingFn)(const wchar_t* passName, const wchar_t* bindingName, int isUav);
void testingSetNullBindingHook(TestingNullBindingFn fn);
// false: FI gets opticalFlowScale 0 (binds no OF resources) and interpolates from the game MVs alone.
void testingSetOpticalFlowEnabled(Context* context, bool enabled);
// reset flag the last successful dispatch() passed to FI and OF / whether a prepare() result is pending.
bool testingLastDispatchReset(const Context* context);
bool testingPrepared(const Context* context);
#endif

}  // namespace tf2fg
