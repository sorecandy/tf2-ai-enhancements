// tf2fg_fsr.cpp - see tf2fg_fsr.h.
//
// Build: lib/build_lib.bat (cl /std:c++17 /O2 /MT /EHs, NDEBUG, TF2FG_FFX_STATIC, no _DEBUG).
// /EHs WITHOUT 'c': the FFX entry points are declared extern "C" but the backend reports D3D11 failures by throwing
// (TIF). With /EHsc the compiler may assume those calls never throw and drop the handlers in ffxCall.
#include <windows.h>  // GetCurrentThreadStackLimits (Windows 8+)

#include "tf2fg_fsr.h"

#include <d3d11_1.h>
#include <d3d11_3.h>

#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <FidelityFX/host/backends/dx11/ffx_dx11.h>

#include <math.h>
#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_DEBUG)
#error "tf2fg_fsr must be built in Release (FFX_ASSERT / __debugbreak would be compiled in)"
#endif
#if !defined(NDEBUG)
#error "tf2fg_fsr must be built with NDEBUG"
#endif

namespace tf2fg {

namespace {

constexpr uint32_t kMaxContexts = 3;       // OF + FI + one spare; FI OF-reset batches need >= 2 x 64 job slots
constexpr uint32_t kTimingSlots = 4;
constexpr uint32_t kMinDisplay = 512;      // OF level-6 pyramid: ceil(W/8) >> 6 must be >= 1
constexpr uint32_t kMaxDim = 16384;
constexpr uint32_t kRejectLogBurst = 32;   // log the first N rejected calls, then every kRejectLogEvery-th
constexpr uint32_t kRejectLogEvery = 1024;

// ReShade 6.8 proxy device: class DECLSPEC_UUID("72299288-2C68-4AD8-945D-2BFB5AA9C609") D3D11Device
// (source/d3d11/d3d11_device.hpp @ v6.8.0); its QueryInterface answers riid == __uuidof(this) with the proxy itself,
// a native D3D11 device answers E_NOINTERFACE.
constexpr GUID kReShadeD3D11DeviceProxy = {0x72299288, 0x2C68, 0x4AD8, {0x94, 0x5D, 0x2B, 0xFB, 0x5A, 0xA9, 0xC6, 0x09}};

template <class T>
inline void safeRelease(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

struct TimingSlot {
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* begin = nullptr;
    ID3D11Query* end = nullptr;
    bool pending = false;
};

}  // namespace

struct Context {
    // device
    ID3D11Device* device = nullptr;
    ID3D11Device1* device1 = nullptr;
    ID3D11DeviceContext* immediate = nullptr;
    ID3D11DeviceContext1* immediate1 = nullptr;
    ID3DDeviceContextState* privateState = nullptr;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    StateMode stateMode = StateMode::SwapContextState;

    // options
    LogFn log = nullptr;
    void* logUser = nullptr;
    float viewSpaceToMeters = 1.0f;
    uint32_t dispatchFlags = 0;

    uint32_t displayW = 0, displayH = 0, maxRenderW = 0, maxRenderH = 0;

    // FFX
    void* scratch = nullptr;
    size_t scratchSize = 0;
    FfxInterface ffxInterface = {};
    FfxOpticalflowContext* of = nullptr;
    bool ofCreated = false;
    FfxFrameInterpolationContext* fi = nullptr;
    bool fiCreated = false;

    // shared resources (owned)
    ID3D11Texture2D* ofVector = nullptr;       // R16G16_SINT ceil(W/8) x ceil(H/8)
    ID3D11Texture2D* ofScd = nullptr;          // R32_UINT 3x1
    uint32_t ofW = 0, ofH = 0;
    ID3D11Texture2D* dilatedDepth = nullptr;   // R32_FLOAT maxRender
    ID3D11Texture2D* dilatedMv = nullptr;      // R16G16_FLOAT maxRender
    ID3D11Texture2D* reconPrevDepth = nullptr; // R32_UINT maxRender

    // colour (owned, R8G8B8A8_TYPELESS so FFX can take UNORM views of sRGB-encoded bits)
    ID3D11Texture2D* curCopy = nullptr;        // SRV
    ID3D11Texture2D* hudCopy = nullptr;        // SRV
    ID3D11Texture2D* outTex = nullptr;         // SRV | UAV

    // frame sequencing
    //   prepared     : a successful prepare() whose result no dispatch() has consumed yet. Cleared at the start of every
    //                  prepare() and by every dispatch() call (whatever its outcome), so a prepare() result can never
    //                  be paired with a later frame's colour.
    //   resetPending : the next successful dispatch() resets FI + OF. Set at creation, by prepare(reset), and by every
    //                  rejected or skipped call (the colour/motion sequence FI + OF saw has a hole).
    bool prepared = false;
    bool resetPending = true;
    uint64_t frameID = 0;
    float frameTimeDeltaMs = 0.0f;
    float cameraNear = 0.0f, cameraFar = 0.0f, cameraFovY = 0.0f;
    uint32_t renderW = 0, renderH = 0;
    // the last successful dispatch(): a frameID that is not +1, or a change of HUD-less presence, forces a reset
    bool haveLastDispatch = false;
    uint64_t lastDispatchFrameID = 0;
    bool lastHadHudless = false;
    bool lastDispatchReset = false;

    bool failed = false;
    bool inCall = false;             // re-entrancy guard (e.g. a log callback calling back into tf2fg)
    uint32_t rejectCount = 0;        // rate limit for "rejected call" log lines
    size_t minStackHeadroom = 256u * 1024u;
    bool stackLogged = false;
    bool swapFailureLogged = false;
    bool opticalFlowDisabled = false;   // setOpticalFlowEnabled(false) / testingSetOpticalFlowEnabled(false)

    // GPU timing
    bool gpuTiming = false;
    TimingSlot timing[kTimingSlots];
    uint32_t timingNext = 0;
    double lastGpuMs = 0.0;
    bool haveGpuMs = false;
};

namespace {

void vlogf(const Context* c, LogLevel level, const char* fmt, va_list args)
{
    if (!c || !c->log || !fmt)
        return;
    char buffer[768];
    const int n = vsnprintf(buffer, sizeof(buffer), fmt, args);
    if (n < 0)
        return;
    buffer[sizeof(buffer) - 1] = 0;
    try {
        c->log(c->logUser, level, buffer);
    } catch (...) {
    }
}

void logf(const Context* c, LogLevel level, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vlogf(c, level, fmt, args);
    va_end(args);
}

// A call refused without touching the GPU (bad argument, bad sequence, too little stack). The context stays usable,
// but the frame sequence FI + OF see now has a hole, so the next successful dispatch() resets both. Logged with a
// rate limit (a persistent caller error must not flood the log from the present thread).
Status reject(Context* c, const char* fmt, ...)
{
    c->resetPending = true;
    const uint32_t n = c->rejectCount++;
    if (n < kRejectLogBurst || (n % kRejectLogEvery) == 0) {
        va_list args;
        va_start(args, fmt);
        vlogf(c, LogLevel::Error, fmt, args);
        va_end(args);
    }
    return Status::InvalidArgument;
}

// Re-entrancy guard for prepare/dispatch/destroy/lastGpuTimeMs. Everything here runs synchronously on the present
// thread; the only way back in is the log callback (or a hook the log callback triggers).
class CallGuard {
public:
    explicit CallGuard(Context* c) : c_(c) { c_->inCall = true; }
    ~CallGuard() { c_->inCall = false; }
    CallGuard(const CallGuard&) = delete;
    CallGuard& operator=(const CallGuard&) = delete;

private:
    Context* c_;
};

// Stack headroom guard. FFX + the D3D11 runtime + driver run on the caller's (game present) thread; a stack overflow
// is fatal and cannot be caught. Logs the headroom once (the first call on a context), refuses when too little is left.
bool stackHeadroomOk(Context* c, const char* what)
{
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    volatile char marker = 0;
    const ULONG_PTR sp = reinterpret_cast<ULONG_PTR>(&marker);
    if (low == 0 || high <= low || sp <= low || sp > high)
        return true;  // limits unknown: nothing to judge against
    const size_t headroom = size_t(sp - low);
    if (!c->stackLogged) {
        c->stackLogged = true;
        logf(c, LogLevel::Info, "%s: thread stack headroom %zu KB of %zu KB reserved (refuse below %zu KB)", what, headroom / 1024,
             size_t(high - low) / 1024, c->minStackHeadroom / 1024);
    }
    return headroom >= c->minStackHeadroom;
}

void setReason(char* out, uint32_t size, const char* fmt, ...)
{
    if (!out || size == 0 || !fmt)
        return;
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(out, size, fmt, args);
    va_end(args);
    if (n < 0)
        out[0] = 0;
    out[size - 1] = 0;
}

inline bool finite(float v) { return isfinite(v) != 0; }

bool isRgba8Family(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

// ---------------------------------------------------------------------------------------------------------------
// Immediate-context state preservation.
//   SwapContextState : the whole FFX batch runs in a private ID3DDeviceContextState; before swapping back the private
//                      state is ClearState()-ed so it never holds references or bindings between calls.
//   ManualComputeSave: CS shader (+class instances), CBs (+first/num constants), SRVs, UAVs, samplers.
// ---------------------------------------------------------------------------------------------------------------
class StateScope {
public:
    explicit StateScope(Context* c) : c_(c)
    {
        if (c_->stateMode == StateMode::SwapContextState && c_->immediate1 && c_->privateState) {
            c_->immediate1->SwapDeviceContextState(c_->privateState, &previous_);
            // SwapDeviceContextState returns void. Only a non-null previous state that is not our private state proves
            // that the caller's state was swapped out; otherwise ClearState()/swap-back in the destructor would wipe
            // or replace the caller's live state. Never ClearState in that case: fall back to the manual CS save for
            // this and every later call.
            if (previous_ && previous_ != c_->privateState) {
                swapped_ = true;
                return;
            }
            safeRelease(previous_);
            c_->stateMode = StateMode::ManualComputeSave;
            if (!c_->swapFailureLogged) {
                c_->swapFailureLogged = true;
                logf(c_, LogLevel::Warn, "SwapDeviceContextState did not swap; using manual CS save/restore from now on");
            }
        }
        saveCompute();
    }

    ~StateScope()
    {
        if (swapped_) {
            c_->immediate->ClearState();  // clears the PRIVATE state only
            c_->immediate1->SwapDeviceContextState(previous_, nullptr);
            safeRelease(previous_);
            return;
        }
        restoreCompute();
    }

    StateScope(const StateScope&) = delete;
    StateScope& operator=(const StateScope&) = delete;

private:
    void saveCompute()
    {
        ID3D11DeviceContext* ctx = c_->immediate;
        numInstances_ = D3D11_SHADER_MAX_INTERFACES;
        ctx->CSGetShader(&cs_, instances_, &numInstances_);
        if (numInstances_ > D3D11_SHADER_MAX_INTERFACES)
            numInstances_ = D3D11_SHADER_MAX_INTERFACES;
        if (c_->immediate1)
            c_->immediate1->CSGetConstantBuffers1(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, cbs_, cbFirst_, cbNum_);
        else
            ctx->CSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, cbs_);
        ctx->CSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, srvs_);
        uavCount_ = (c_->featureLevel >= D3D_FEATURE_LEVEL_11_1) ? D3D11_1_UAV_SLOT_COUNT : D3D11_PS_CS_UAV_REGISTER_COUNT;
        ctx->CSGetUnorderedAccessViews(0, uavCount_, uavs_);
        ctx->CSGetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers_);
        // FFX binds caller textures (depth / motion vectors) as CS SRVs: a CS UAV binding of the same resource would
        // make the runtime drop the SRV. Start from an empty UAV set.
        static ID3D11UnorderedAccessView* const nullUavs[D3D11_1_UAV_SLOT_COUNT] = {};
        ctx->CSSetUnorderedAccessViews(0, uavCount_, nullUavs, nullptr);
    }

    void restoreCompute()
    {
        ID3D11DeviceContext* ctx = c_->immediate;
        static ID3D11ShaderResourceView* const nullSrvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
        static ID3D11UnorderedAccessView* const nullUavs[D3D11_1_UAV_SLOT_COUNT] = {};
        // clear first so restored inputs are not rejected by hazards with FFX leftovers
        ctx->CSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, nullSrvs);
        ctx->CSSetUnorderedAccessViews(0, uavCount_, nullUavs, nullptr);

        ctx->CSSetShader(cs_, numInstances_ ? instances_ : nullptr, numInstances_);
        if (c_->immediate1)
            c_->immediate1->CSSetConstantBuffers1(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, cbs_, cbFirst_, cbNum_);
        else
            ctx->CSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, cbs_);
        ctx->CSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, srvs_);
        UINT keepCounters[D3D11_1_UAV_SLOT_COUNT];
        for (UINT i = 0; i < D3D11_1_UAV_SLOT_COUNT; ++i)
            keepCounters[i] = UINT(-1);
        ctx->CSSetUnorderedAccessViews(0, uavCount_, uavs_, keepCounters);
        ctx->CSSetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers_);

        safeRelease(cs_);
        for (UINT i = 0; i < numInstances_; ++i) safeRelease(instances_[i]);
        for (auto& p : cbs_) safeRelease(p);
        for (auto& p : srvs_) safeRelease(p);
        for (auto& p : uavs_) safeRelease(p);
        for (auto& p : samplers_) safeRelease(p);
    }

    Context* c_;
    bool swapped_ = false;
    ID3DDeviceContextState* previous_ = nullptr;

    ID3D11ComputeShader* cs_ = nullptr;
    ID3D11ClassInstance* instances_[D3D11_SHADER_MAX_INTERFACES] = {};
    UINT numInstances_ = 0;
    ID3D11Buffer* cbs_[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
    UINT cbFirst_[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
    UINT cbNum_[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
    ID3D11ShaderResourceView* srvs_[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    ID3D11UnorderedAccessView* uavs_[D3D11_1_UAV_SLOT_COUNT] = {};
    UINT uavCount_ = 0;
    ID3D11SamplerState* samplers_[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
};

// Runs one FFX call; converts C++ exceptions (the backend's TIF throws HRESULTs as long) into Status::Exception.
// noinline + /EHs (build_lib.bat): the try region must survive whatever the optimizer does with the extern "C" call.
template <class F>
__declspec(noinline) Status ffxCall(Context* c, const char* what, F&& fn)
{
    FfxErrorCode err = FFX_OK;
    try {
        err = fn();
    } catch (long hr) {
        logf(c, LogLevel::Error, "%s: D3D11 call failed inside FFX (HRESULT 0x%08lx)", what, (unsigned long)hr);
        return Status::Exception;
    } catch (...) {
        logf(c, LogLevel::Error, "%s: C++ exception inside FFX", what);
        return Status::Exception;
    }
    if (err != FFX_OK) {
        logf(c, LogLevel::Error, "%s failed: FfxErrorCode 0x%08x", what, (unsigned)err);
        return Status::FfxError;
    }
    return Status::Ok;
}

HRESULT createTexture(ID3D11Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind, ID3D11Texture2D** out)
{
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    return dev->CreateTexture2D(&d, nullptr, out);
}

// Zero a texture through a temporary UAV (must be called inside a StateScope).
void clearTextureUint(Context* c, ID3D11Texture2D* tex, DXGI_FORMAT viewFormat)
{
    if (!tex)
        return;
    D3D11_UNORDERED_ACCESS_VIEW_DESC u = {};
    u.Format = viewFormat;
    u.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    ID3D11UnorderedAccessView* uav = nullptr;
    if (SUCCEEDED(c->device->CreateUnorderedAccessView(tex, &u, &uav)) && uav) {
        const UINT zero[4] = {0, 0, 0, 0};
        c->immediate->ClearUnorderedAccessViewUint(uav, zero);
        uav->Release();
    }
}

FfxResource makeResource(ID3D11Resource* res, FfxSurfaceFormat fmt, uint32_t w, uint32_t h, FfxResourceUsage usage,
                         FfxResourceStates state)
{
    FfxResource r = {};
    r.resource = res;
    r.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = fmt;
    r.description.width = w;
    r.description.height = h;
    r.description.depth = 1;
    r.description.mipCount = 1;  // every registered texture has mipCount >= 1 (0 would mean "no UAV" upstream)
    r.description.flags = FFX_RESOURCE_FLAGS_NONE;
    r.description.usage = usage;
    r.state = state;
    return r;
}

// Release everything GPU-side; the Context struct itself survives (destroy() frees it).
void teardown(Context* c)
{
    if (!c)
        return;
    try {
        if (c->fiCreated || c->ofCreated) {
            StateScope scope(c);
            if (c->fiCreated) {
                ffxCall(c, "ffxFrameInterpolationContextDestroy", [&] { return ffxFrameInterpolationContextDestroy(c->fi); });
                c->fiCreated = false;
            }
            if (c->ofCreated) {
                ffxCall(c, "ffxOpticalflowContextDestroy", [&] { return ffxOpticalflowContextDestroy(c->of); });
                c->ofCreated = false;
            }
        }
    } catch (...) {
        c->fiCreated = false;
        c->ofCreated = false;
    }
    free(c->fi);
    c->fi = nullptr;
    free(c->of);
    c->of = nullptr;
    free(c->scratch);
    c->scratch = nullptr;
    c->scratchSize = 0;

    safeRelease(c->ofVector);
    safeRelease(c->ofScd);
    safeRelease(c->dilatedDepth);
    safeRelease(c->dilatedMv);
    safeRelease(c->reconPrevDepth);
    safeRelease(c->curCopy);
    safeRelease(c->hudCopy);
    safeRelease(c->outTex);
    for (auto& t : c->timing) {
        safeRelease(t.disjoint);
        safeRelease(t.begin);
        safeRelease(t.end);
        t.pending = false;
    }
}

Status fail(Context* c, Status s, const char* what)
{
    if (c) {
        if (!c->failed)
            logf(c, LogLevel::Error, "%s failed (%s); FSR frame generation disabled until recreated", what, statusName(s));
        c->failed = true;
        teardown(c);
    }
    return s;
}

void pollTiming(Context* c)
{
    for (auto& t : c->timing) {
        if (!t.pending)
            continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
        if (c->immediate->GetData(t.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        UINT64 b = 0, e = 0;
        if (c->immediate->GetData(t.begin, &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            c->immediate->GetData(t.end, &e, sizeof(e), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        t.pending = false;
        if (!dj.Disjoint && dj.Frequency > 0 && e >= b) {
            c->lastGpuMs = double(e - b) * 1000.0 / double(dj.Frequency);
            c->haveGpuMs = true;
        }
    }
}

// Validates a caller colour texture. Returns false + logs on mismatch.
// Validates a caller colour texture. Returns false + a reason (the caller logs it through reject()).
bool checkColor(const Context* c, ID3D11Texture2D* tex, const char* name, char* why, size_t whySize)
{
    D3D11_TEXTURE2D_DESC d = {};
    tex->GetDesc(&d);
    if (!isRgba8Family(d.Format)) {
        snprintf(why, whySize, "%s: format %u not in the RGBA8 family (R8G8B8A8_UNORM/_SRGB/_TYPELESS)", name, (unsigned)d.Format);
        return false;
    }
    if (d.Width != c->displayW || d.Height != c->displayH || d.SampleDesc.Count != 1 || d.ArraySize != 1 || d.MipLevels < 1) {
        snprintf(why, whySize, "%s: %ux%u samples %u array %u, expected %ux%u single-sample non-array", name, d.Width, d.Height,
                 d.SampleDesc.Count, d.ArraySize, c->displayW, c->displayH);
        return false;
    }
    return true;
}

}  // namespace

// =================================================================================================================
const char* statusName(Status s)
{
    switch (s) {
    case Status::Ok: return "Ok";
    case Status::InvalidArgument: return "InvalidArgument";
    case Status::Unsupported: return "Unsupported";
    case Status::NotInitialized: return "NotInitialized";
    case Status::FfxError: return "FfxError";
    case Status::Exception: return "Exception";
    case Status::DeviceError: return "DeviceError";
    case Status::Failed: return "Failed";
    }
    return "Unknown";
}

Status checkDeviceSupport(ID3D11Device* device, char* reasonOut, uint32_t reasonSize)
{
    setReason(reasonOut, reasonSize, "ok");
    if (!device) {
        setReason(reasonOut, reasonSize, "null device");
        return Status::InvalidArgument;
    }
    try {
        const D3D_FEATURE_LEVEL fl = device->GetFeatureLevel();
        if (fl < D3D_FEATURE_LEVEL_11_1) {
            setReason(reasonOut, reasonSize,
                      "device feature level 0x%x < 11_1: the FI inpainting pyramid passes bind 13 UAVs (FL 11_0 allows 8)",
                      (unsigned)fl);
            return Status::Unsupported;
        }

        ID3D11Device1* device1 = nullptr;
        if (FAILED(device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1))) || !device1) {
            setReason(reasonOut, reasonSize, "ID3D11Device1 not available (D3D11.1 runtime required)");
            return Status::Unsupported;
        }
        device1->Release();

        D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2 = {};
        if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof(o2))) || !o2.TypedUAVLoadAdditionalFormats) {
            setReason(reasonOut, reasonSize, "TypedUAVLoadAdditionalFormats not supported");
            return Status::Unsupported;
        }

        // typed UAV loads found in the compiled shaders (shaders/build_shaders.py asm scan)
        const DXGI_FORMAT typedLoad[] = {DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8G8B8A8_UNORM,
                                         DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_FLOAT};
        for (DXGI_FORMAT f : typedLoad) {
            D3D11_FEATURE_DATA_FORMAT_SUPPORT2 s2 = {f, 0};
            if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &s2, sizeof(s2))) ||
                !(s2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD)) {
                setReason(reasonOut, reasonSize, "UAV typed load not supported for DXGI format %u", (unsigned)f);
                return Status::Unsupported;
            }
        }

        // every format FI / OF write through typed UAVs
        const DXGI_FORMAT typedStore[] = {DXGI_FORMAT_R32_UINT,       DXGI_FORMAT_R32_FLOAT,          DXGI_FORMAT_R16G16_FLOAT,
                                          DXGI_FORMAT_R16G16_SINT,    DXGI_FORMAT_R8_UNORM,           DXGI_FORMAT_R8G8_UNORM,
                                          DXGI_FORMAT_R8_UINT,        DXGI_FORMAT_R8G8B8A8_UNORM,     DXGI_FORMAT_R16G16B16A16_FLOAT};
        for (DXGI_FORMAT f : typedStore) {
            D3D11_FEATURE_DATA_FORMAT_SUPPORT s1 = {f, 0};
            D3D11_FEATURE_DATA_FORMAT_SUPPORT2 s2 = {f, 0};
            if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT, &s1, sizeof(s1))) ||
                !(s1.OutFormatSupport & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) ||
                FAILED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &s2, sizeof(s2))) ||
                !(s2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE)) {
                setReason(reasonOut, reasonSize, "typed UAV store not supported for DXGI format %u", (unsigned)f);
                return Status::Unsupported;
            }
        }
    } catch (...) {
        setReason(reasonOut, reasonSize, "exception during capability check");
        return Status::Exception;
    }
    return Status::Ok;
}

// =================================================================================================================
Status create(ID3D11Device* nativeDevice, uint32_t displayWidth, uint32_t displayHeight, uint32_t maxRenderWidth,
              uint32_t maxRenderHeight, const CreateOptions* options, Context** outContext)
{
    if (!outContext)
        return Status::InvalidArgument;
    *outContext = nullptr;

    CreateOptions opts;
    if (options)
        opts = *options;

    // a throwaway context just for logging before the real one exists
    Context logOnly;
    logOnly.log = opts.log;
    logOnly.logUser = opts.logUser;

    if (!nativeDevice) {
        logf(&logOnly, LogLevel::Error, "create: null device");
        return Status::InvalidArgument;
    }
    // ReShade hooks ID3D11Texture2D::GetDevice to return its PROXY device; every FFX call made through the proxy would
    // raise add-on events (bind/dispatch/copy) into every loaded add-on. Only the native device is accepted.
    {
        IUnknown* proxy = nullptr;
        if (SUCCEEDED(nativeDevice->QueryInterface(kReShadeD3D11DeviceProxy, reinterpret_cast<void**>(&proxy))) && proxy) {
            proxy->Release();
            logf(&logOnly, LogLevel::Error, "create: device is ReShade's proxy D3D11Device; pass reshade::api::device::get_native()");
            return Status::InvalidArgument;
        }
    }
    if (displayWidth < kMinDisplay || displayHeight < kMinDisplay || displayWidth > kMaxDim || displayHeight > kMaxDim ||
        maxRenderWidth < 64 || maxRenderHeight < 64 || maxRenderWidth > displayWidth || maxRenderHeight > displayHeight) {
        logf(&logOnly, LogLevel::Error, "create: bad sizes display %ux%u maxRender %ux%u (display >= %u, 64 <= render <= display)",
             displayWidth, displayHeight, maxRenderWidth, maxRenderHeight, kMinDisplay);
        return Status::InvalidArgument;
    }
    if (!finite(opts.viewSpaceToMeters) || opts.viewSpaceToMeters <= 0.0f) {
        // was silently replaced by 1.0 (metres), which is ~39x off for Source units
        logf(&logOnly, LogLevel::Error, "create: viewSpaceToMeters %g must be finite and > 0 (Source units: 0.0254)", opts.viewSpaceToMeters);
        return Status::InvalidArgument;
    }
    if (opts.minStackHeadroomKB < 64 || opts.minStackHeadroomKB > 65536) {
        logf(&logOnly, LogLevel::Error, "create: minStackHeadroomKB %u outside 64..65536", opts.minStackHeadroomKB);
        return Status::InvalidArgument;
    }

    char reason[256] = {};
    const Status caps = checkDeviceSupport(nativeDevice, reason, sizeof(reason));
    if (caps != Status::Ok) {
        logf(&logOnly, LogLevel::Error, "create: device not supported: %s", reason);
        return caps;
    }

    Context* c = new (std::nothrow) Context();
    if (!c)
        return Status::Failed;

    try {
        c->log = opts.log;
        c->logUser = opts.logUser;
        c->viewSpaceToMeters = opts.viewSpaceToMeters;
        c->minStackHeadroom = size_t(opts.minStackHeadroomKB) * 1024u;
        c->stateMode = opts.stateMode;
        c->gpuTiming = opts.gpuTiming;
        c->dispatchFlags = (opts.debugTearLines ? FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_TEAR_LINES : 0u) |
                           (opts.debugView ? FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_VIEW : 0u);
        c->displayW = displayWidth;
        c->displayH = displayHeight;
        c->maxRenderW = maxRenderWidth;
        c->maxRenderH = maxRenderHeight;

        c->device = nativeDevice;
        c->device->AddRef();
        c->featureLevel = c->device->GetFeatureLevel();
        if (FAILED(c->device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&c->device1))) || !c->device1) {
            destroy(c);
            return Status::Unsupported;
        }
        c->device->GetImmediateContext(&c->immediate);
        if (!c->immediate) {
            destroy(c);
            return Status::DeviceError;
        }
        if (FAILED(c->immediate->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c->immediate1))))
            c->immediate1 = nullptr;

        if (c->stateMode == StateMode::SwapContextState) {
            HRESULT hr = E_FAIL;
            if (c->immediate1) {
                const UINT flags = (c->device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
                                       ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0u;
                D3D_FEATURE_LEVEL fl = c->featureLevel;
                D3D_FEATURE_LEVEL chosen = D3D_FEATURE_LEVEL_11_0;
                hr = c->device1->CreateDeviceContextState(flags, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), &chosen,
                                                          &c->privateState);
                if (SUCCEEDED(hr) && chosen != c->featureLevel) {
                    safeRelease(c->privateState);
                    hr = E_FAIL;
                }
            }
            if (FAILED(hr) || !c->privateState) {
                logf(c, LogLevel::Warn, "CreateDeviceContextState failed (0x%08lx); falling back to manual CS save/restore",
                     (unsigned long)hr);
                c->stateMode = StateMode::ManualComputeSave;
            }
        }

        // ---- FFX interface (one scratch buffer shared by OF + FI) ----
        c->scratchSize = ffxGetScratchMemorySizeDX11(kMaxContexts);
        c->scratch = calloc(1, c->scratchSize);  // zero-allocated: the backend relies on refCount == 0
        c->of = static_cast<FfxOpticalflowContext*>(calloc(1, sizeof(FfxOpticalflowContext)));
        c->fi = static_cast<FfxFrameInterpolationContext*>(calloc(1, sizeof(FfxFrameInterpolationContext)));
        if (!c->scratch || !c->of || !c->fi) {
            destroy(c);
            return Status::Failed;
        }

        Status s = Status::Ok;
        {
            StateScope scope(c);

            s = ffxCall(c, "ffxGetInterfaceDX11", [&] {
                return ffxGetInterfaceDX11(&c->ffxInterface, reinterpret_cast<FfxDevice>(c->device), c->scratch, c->scratchSize,
                                           kMaxContexts);
            });

            // ---- optical flow ----
            if (s == Status::Ok) {
                FfxOpticalflowContextDescription ofDesc = {};
                ofDesc.backendInterface = c->ffxInterface;
                ofDesc.flags = 0;
                ofDesc.resolution = {displayWidth, displayHeight};
                s = ffxCall(c, "ffxOpticalflowContextCreate", [&] { return ffxOpticalflowContextCreate(c->of, &ofDesc); });
                c->ofCreated = (s == Status::Ok);  // a failed create cleans up after itself (TF2FG PATCH O3)
            }
            if (s == Status::Ok) {
                FfxOpticalflowSharedResourceDescriptions ofShared = {};
                s = ffxCall(c, "ffxOpticalflowGetSharedResourceDescriptions",
                            [&] { return ffxOpticalflowGetSharedResourceDescriptions(c->of, &ofShared); });
                if (s == Status::Ok) {
                    c->ofW = ofShared.opticalFlowVector.resourceDescription.width;
                    c->ofH = ofShared.opticalFlowVector.resourceDescription.height;
                    if (c->ofW == 0 || c->ofH == 0 || c->ofW > kMaxDim || c->ofH > kMaxDim)
                        s = Status::Failed;
                }
                if (s == Status::Ok) {
                    const UINT bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                    if (FAILED(createTexture(c->device, c->ofW, c->ofH, DXGI_FORMAT_R16G16_SINT, bind, &c->ofVector)) ||
                        FAILED(createTexture(c->device, 3, 1, DXGI_FORMAT_R32_UINT, bind, &c->ofScd)))
                        s = Status::DeviceError;
                }
            }

            // ---- frame interpolation ----
            if (s == Status::Ok) {
                FfxFrameInterpolationContextDescription fiDesc = {};
                fiDesc.flags = 0;  // standard depth, finite far, render-res MVs, MVs without jitter
                fiDesc.maxRenderSize = {maxRenderWidth, maxRenderHeight};
                fiDesc.displaySize = {displayWidth, displayHeight};
                fiDesc.backBufferFormat = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
                fiDesc.backendInterface = c->ffxInterface;
                s = ffxCall(c, "ffxFrameInterpolationContextCreate", [&] { return ffxFrameInterpolationContextCreate(c->fi, &fiDesc); });
                c->fiCreated = (s == Status::Ok);  // a failed create cleans up after itself (TF2FG PATCH F3)
            }
            if (s == Status::Ok) {
                const UINT bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                if (FAILED(createTexture(c->device, maxRenderWidth, maxRenderHeight, DXGI_FORMAT_R32_FLOAT, bind, &c->dilatedDepth)) ||
                    FAILED(createTexture(c->device, maxRenderWidth, maxRenderHeight, DXGI_FORMAT_R16G16_FLOAT, bind, &c->dilatedMv)) ||
                    FAILED(createTexture(c->device, maxRenderWidth, maxRenderHeight, DXGI_FORMAT_R32_UINT, bind, &c->reconPrevDepth)) ||
                    FAILED(createTexture(c->device, displayWidth, displayHeight, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE, &c->curCopy)) ||
                    FAILED(createTexture(c->device, displayWidth, displayHeight, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE, &c->hudCopy)) ||
                    FAILED(createTexture(c->device, displayWidth, displayHeight, DXGI_FORMAT_R8G8B8A8_TYPELESS, bind, &c->outTex)))
                    s = Status::DeviceError;
            }
            if (s == Status::Ok) {
                clearTextureUint(c, c->ofVector, DXGI_FORMAT_R16G16_SINT);
                clearTextureUint(c, c->ofScd, DXGI_FORMAT_R32_UINT);
                clearTextureUint(c, c->dilatedDepth, DXGI_FORMAT_R32_FLOAT);
                clearTextureUint(c, c->dilatedMv, DXGI_FORMAT_R16G16_FLOAT);
                clearTextureUint(c, c->reconPrevDepth, DXGI_FORMAT_R32_UINT);
                clearTextureUint(c, c->outTex, DXGI_FORMAT_R8G8B8A8_UNORM);
            }
            if (s == Status::Ok && c->gpuTiming) {
                D3D11_QUERY_DESC dj = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
                D3D11_QUERY_DESC ts = {D3D11_QUERY_TIMESTAMP, 0};
                for (auto& t : c->timing) {
                    if (FAILED(c->device->CreateQuery(&dj, &t.disjoint)) || FAILED(c->device->CreateQuery(&ts, &t.begin)) ||
                        FAILED(c->device->CreateQuery(&ts, &t.end))) {
                        logf(c, LogLevel::Warn, "timestamp queries unavailable; GPU timing disabled");
                        c->gpuTiming = false;
                        break;
                    }
                }
            }
        }

        if (s != Status::Ok) {
            logf(c, LogLevel::Error, "create failed (%s)", statusName(s));
            destroy(c);
            return s;
        }
    } catch (...) {
        logf(c, LogLevel::Error, "create: exception");
        destroy(c);
        return Status::Exception;
    }

    logf(c, LogLevel::Info, "created: display %ux%u maxRender %ux%u FL 0x%x state %s, OF %ux%u", displayWidth, displayHeight,
         maxRenderWidth, maxRenderHeight, (unsigned)c->featureLevel,
         c->stateMode == StateMode::SwapContextState ? "SwapDeviceContextState" : "manual CS save", c->ofW, c->ofH);
    *outContext = c;
    return Status::Ok;
}

// =================================================================================================================
Status prepare(Context* c, const PrepareParams& p)
{
    if (!c || c->failed || !c->fiCreated)
        return Status::NotInitialized;
    if (c->inCall)
        return Status::InvalidArgument;  // re-entered from the log callback: refuse, do not touch anything
    CallGuard guard(c);

    // Every prepare() invalidates the previous one. A prepare() result that no dispatch() consumed means a frame was
    // skipped (the colour sequence has a hole) -> reset on the next dispatch().
    if (c->prepared)
        c->resetPending = true;
    c->prepared = false;
    try {
        // ---- validation (nothing touched yet: errors here do not disable the context) ----
        if (!stackHeadroomOk(c, "prepare"))
            return reject(c, "prepare: less than %zu KB of thread stack left; frame skipped", c->minStackHeadroom / 1024);
        if (!p.depth || !p.motionVectors)
            return reject(c, "prepare: null depth or motion vectors");
        if (p.renderWidth == 0 || p.renderHeight == 0 || p.renderWidth > c->maxRenderW || p.renderHeight > c->maxRenderH)
            return reject(c, "prepare: render size %ux%u outside 1..%ux%u", p.renderWidth, p.renderHeight, c->maxRenderW, c->maxRenderH);
        if (!finite(p.cameraNear) || !finite(p.cameraFar) || p.cameraNear <= 0.0f || p.cameraFar <= 0.0f ||
            p.cameraNear == p.cameraFar || !finite(p.cameraFovY) || p.cameraFovY <= 0.0f || p.cameraFovY >= 3.14159f)
            return reject(c, "prepare: bad camera near %g far %g fovY %g", p.cameraNear, p.cameraFar, p.cameraFovY);
        D3D11_TEXTURE2D_DESC dd = {}, md = {};
        p.depth->GetDesc(&dd);
        p.motionVectors->GetDesc(&md);
        if ((dd.Format != DXGI_FORMAT_R32_FLOAT && dd.Format != DXGI_FORMAT_R32_TYPELESS) || !(dd.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
            dd.Width != p.renderWidth || dd.Height != p.renderHeight || dd.SampleDesc.Count != 1 || dd.ArraySize != 1)
            return reject(c, "prepare: depth must be R32_FLOAT/R32_TYPELESS SRV %ux%u (got fmt %u %ux%u bind 0x%x)", p.renderWidth,
                          p.renderHeight, (unsigned)dd.Format, dd.Width, dd.Height, dd.BindFlags);
        if ((md.Format != DXGI_FORMAT_R16G16_FLOAT && md.Format != DXGI_FORMAT_R16G16_TYPELESS) || !(md.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
            md.Width != p.renderWidth || md.Height != p.renderHeight || md.SampleDesc.Count != 1 || md.ArraySize != 1)
            return reject(c, "prepare: motion vectors must be R16G16_FLOAT/_TYPELESS SRV %ux%u (got fmt %u %ux%u bind 0x%x)", p.renderWidth,
                          p.renderHeight, (unsigned)md.Format, md.Width, md.Height, md.BindFlags);

        float dt = finite(p.frameTimeDeltaMs) ? p.frameTimeDeltaMs : 0.0f;
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 1000.0f) dt = 1000.0f;

        FfxFrameInterpolationPrepareDescription d = {};
        d.flags = 0;
        d.commandList = reinterpret_cast<FfxCommandList>(c->immediate);
        d.renderSize = {p.renderWidth, p.renderHeight};
        d.jitterOffset = {finite(p.jitterX) ? p.jitterX : 0.0f, finite(p.jitterY) ? p.jitterY : 0.0f};
        d.motionVectorScale = {1.0f, 1.0f};  // render pixels; FI divides by renderSize
        d.frameTimeDelta = dt;
        d.cameraNear = p.cameraNear;
        d.cameraFar = p.cameraFar;
        d.viewSpaceToMetersFactor = c->viewSpaceToMeters;
        d.cameraFovAngleVertical = p.cameraFovY;
        d.depth = makeResource(p.depth, FFX_SURFACE_FORMAT_R32_FLOAT, p.renderWidth, p.renderHeight, FFX_RESOURCE_USAGE_READ_ONLY,
                               FFX_RESOURCE_STATE_COMPUTE_READ);
        d.motionVectors = makeResource(p.motionVectors, FFX_SURFACE_FORMAT_R16G16_FLOAT, p.renderWidth, p.renderHeight,
                                       FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ);
        d.frameID = p.frameID;
        d.dilatedDepth = makeResource(c->dilatedDepth, FFX_SURFACE_FORMAT_R32_FLOAT, c->maxRenderW, c->maxRenderH,
                                      FFX_RESOURCE_USAGE_UAV, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        d.dilatedMotionVectors = makeResource(c->dilatedMv, FFX_SURFACE_FORMAT_R16G16_FLOAT, c->maxRenderW, c->maxRenderH,
                                              FFX_RESOURCE_USAGE_UAV, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        d.reconstructedPrevDepth = makeResource(c->reconPrevDepth, FFX_SURFACE_FORMAT_R32_UINT, c->maxRenderW, c->maxRenderH,
                                                FFX_RESOURCE_USAGE_UAV, FFX_RESOURCE_STATE_UNORDERED_ACCESS);

        Status s;
        {
            StateScope scope(c);
            s = ffxCall(c, "ffxFrameInterpolationPrepare", [&] { return ffxFrameInterpolationPrepare(c->fi, &d); });
        }
        if (s != Status::Ok)
            return fail(c, s, "prepare");

        c->prepared = true;
        c->resetPending = c->resetPending || p.reset;
        c->frameID = p.frameID;
        c->frameTimeDeltaMs = dt;
        c->cameraNear = p.cameraNear;
        c->cameraFar = p.cameraFar;
        c->cameraFovY = p.cameraFovY;
        c->renderW = p.renderWidth;
        c->renderH = p.renderHeight;
        return Status::Ok;
    } catch (...) {
        return fail(c, Status::Exception, "prepare");
    }
}

// =================================================================================================================
Status dispatch(Context* c, const DispatchParams& p)
{
    if (!c || c->failed || !c->fiCreated || !c->ofCreated)
        return Status::NotInitialized;
    if (c->inCall)
        return Status::InvalidArgument;  // re-entered from the log callback: refuse, do not touch anything
    CallGuard guard(c);

    // A prepare() result is consumed by exactly one dispatch() call, whatever its outcome: a rejected dispatch(N)
    // must not leave frame N's depth/MVs/frameID behind for dispatch(N+1).
    const bool wasPrepared = c->prepared;
    c->prepared = false;
    try {
        if (!wasPrepared)
            return reject(c, "dispatch: no successful prepare() for this frame");
        if (!p.currentBackBuffer || !p.output)
            return reject(c, "dispatch: null currentBackBuffer or output");
        if (p.hudless && p.hudless == p.output)
            return reject(c, "dispatch: output must not alias hudless");
        if (p.transfer != Transfer::Srgb)
            return reject(c, "dispatch: only the sRGB transfer function is supported for RGBA8 colour");
        char why[256] = {};
        if (!checkColor(c, p.currentBackBuffer, "currentBackBuffer", why, sizeof(why)) || !checkColor(c, p.output, "output", why, sizeof(why)) ||
            (p.hudless && !checkColor(c, p.hudless, "hudless", why, sizeof(why))))
            return reject(c, "dispatch: %s", why);
        if (!stackHeadroomOk(c, "dispatch"))
            return reject(c, "dispatch: less than %zu KB of thread stack left; frame skipped", c->minStackHeadroom / 1024);

        // Reset FI AND OF whenever the input stream is discontinuous. FI resets itself on a frameID gap but OF has no
        // frameID (it would compare against a stale luma pyramid / flow prior), and a HUD-less presence toggle
        // (menus/loading pass none) makes FI's previous interpolation source and OF's previous luma a different
        // kind of image than the current one.
        const bool hasHudless = p.hudless != nullptr;
        const bool discontinuous = !c->haveLastDispatch || c->frameID != c->lastDispatchFrameID + 1 || hasHudless != c->lastHadHudless;
        const bool reset = c->resetPending || p.reset || discontinuous;

        const FfxResource cur = makeResource(c->curCopy, FFX_SURFACE_FORMAT_R8G8B8A8_UNORM, c->displayW, c->displayH,
                                             FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ);
        const FfxResource hud = makeResource(p.hudless ? c->hudCopy : nullptr, FFX_SURFACE_FORMAT_R8G8B8A8_UNORM, c->displayW,
                                             c->displayH, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ);
        const FfxResource out = makeResource(c->outTex, FFX_SURFACE_FORMAT_R8G8B8A8_UNORM, c->displayW, c->displayH,
                                             FFX_RESOURCE_USAGE_UAV, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        const FfxResource ofVec = makeResource(c->ofVector, FFX_SURFACE_FORMAT_R16G16_SINT, c->ofW, c->ofH, FFX_RESOURCE_USAGE_UAV,
                                               FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        const FfxResource ofScd = makeResource(c->ofScd, FFX_SURFACE_FORMAT_R32_UINT, 3, 1, FFX_RESOURCE_USAGE_UAV,
                                               FFX_RESOURCE_STATE_UNORDERED_ACCESS);

        FfxOpticalflowDispatchDescription ofd = {};
        ofd.commandList = reinterpret_cast<FfxCommandList>(c->immediate);
        ofd.color = p.hudless ? hud : cur;
        ofd.opticalFlowVector = ofVec;
        ofd.opticalFlowSCD = ofScd;
        ofd.reset = reset;
        ofd.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        ofd.minMaxLuminance = {0.0f, 1.0f};

        FfxFrameInterpolationDispatchDescription fid = {};
        fid.flags = c->dispatchFlags;
        fid.commandList = reinterpret_cast<FfxCommandList>(c->immediate);
        fid.displaySize = {c->displayW, c->displayH};
        fid.renderSize = {c->renderW, c->renderH};
        fid.currentBackBuffer = cur;
        fid.currentBackBuffer_HUDLess = hud;  // resource == nullptr -> HUDLessAttachedFactor 0
        fid.output = out;
        fid.interpolationRect = {0, 0, int32_t(c->displayW), int32_t(c->displayH)};
        fid.opticalFlowVector = ofVec;
        fid.opticalFlowSceneChangeDetection = ofScd;
        fid.opticalFlowBufferSize = {c->ofW, c->ofH};
        fid.opticalFlowScale = {1.0f / float(c->displayW), 1.0f / float(c->displayH)};  // displayW/H >= kMinDisplay
        if (c->opticalFlowDisabled)
            fid.opticalFlowScale = {0.0f, 0.0f};  // FI then binds no OF resources and relies on the game MVs alone
        fid.opticalFlowBlockSize = 8;
        fid.cameraNear = c->cameraNear;
        fid.cameraFar = c->cameraFar;
        fid.cameraFovAngleVertical = c->cameraFovY;
        fid.viewSpaceToMetersFactor = c->viewSpaceToMeters;
        fid.frameTimeDelta = c->frameTimeDeltaMs;
        fid.reset = reset;
        fid.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        fid.minMaxLuminance[0] = 0.0f;
        fid.minMaxLuminance[1] = 1.0f;
        fid.frameID = c->frameID;
        fid.dilatedDepth = makeResource(c->dilatedDepth, FFX_SURFACE_FORMAT_R32_FLOAT, c->maxRenderW, c->maxRenderH,
                                        FFX_RESOURCE_USAGE_UAV, FFX_RESOURCE_STATE_COMPUTE_READ);
        fid.dilatedMotionVectors = makeResource(c->dilatedMv, FFX_SURFACE_FORMAT_R16G16_FLOAT, c->maxRenderW, c->maxRenderH,
                                                FFX_RESOURCE_USAGE_UAV, FFX_RESOURCE_STATE_COMPUTE_READ);
        fid.reconstructedPrevDepth = makeResource(c->reconPrevDepth, FFX_SURFACE_FORMAT_R32_UINT, c->maxRenderW, c->maxRenderH,
                                                  FFX_RESOURCE_USAGE_UAV, FFX_RESOURCE_STATE_COMPUTE_READ);

        Status s = Status::Ok;
        {
            StateScope scope(c);

            // raw bit copies: sRGB-encoded bytes stay sRGB-encoded, FI decodes them itself (RawRGBToLinear)
            c->immediate->CopySubresourceRegion(c->curCopy, 0, 0, 0, 0, p.currentBackBuffer, 0, nullptr);
            if (p.hudless)
                c->immediate->CopySubresourceRegion(c->hudCopy, 0, 0, 0, 0, p.hudless, 0, nullptr);

            TimingSlot* slot = nullptr;
            if (c->gpuTiming) {
                pollTiming(c);
                TimingSlot& t = c->timing[c->timingNext % kTimingSlots];
                if (!t.pending) {
                    slot = &t;
                    c->timingNext = (c->timingNext + 1) % kTimingSlots;
                    c->immediate->Begin(slot->disjoint);
                    c->immediate->End(slot->begin);
                }
            }

            s = ffxCall(c, "ffxOpticalflowContextDispatch", [&] { return ffxOpticalflowContextDispatch(c->of, &ofd); });
            if (s == Status::Ok)
                s = ffxCall(c, "ffxFrameInterpolationDispatch", [&] { return ffxFrameInterpolationDispatch(c->fi, &fid); });

            if (slot) {
                c->immediate->End(slot->end);
                c->immediate->End(slot->disjoint);
                slot->pending = true;
            }

            if (s == Status::Ok)
                c->immediate->CopySubresourceRegion(p.output, 0, 0, 0, 0, c->outTex, 0, nullptr);
        }
        if (s != Status::Ok)
            return fail(c, s, "dispatch");

        c->resetPending = false;
        c->haveLastDispatch = true;
        c->lastDispatchFrameID = c->frameID;
        c->lastHadHudless = hasHudless;
        c->lastDispatchReset = reset;
        return Status::Ok;
    } catch (...) {
        return fail(c, Status::Exception, "dispatch");
    }
}

// =================================================================================================================
void destroy(Context* c)
{
    if (!c)
        return;
    if (c->inCall) {
        // called from the log callback while prepare()/dispatch() is still running on this context: freeing it now
        // would pull the context out from under that call. Refused; the caller must destroy() again afterwards.
        logf(c, LogLevel::Error, "destroy: called re-entrantly (from the log callback); ignored");
        return;
    }
    c->inCall = true;
    try {
        teardown(c);
    } catch (...) {
    }
    safeRelease(c->privateState);
    safeRelease(c->immediate1);
    safeRelease(c->immediate);
    safeRelease(c->device1);
    safeRelease(c->device);
    c->log = nullptr;
    delete c;
}

bool lastGpuTimeMs(Context* c, double* milliseconds)
{
    if (!c || !milliseconds || c->failed || !c->gpuTiming || c->inCall)
        return false;
    try {
        pollTiming(c);
    } catch (...) {
        return false;
    }
    if (!c->haveGpuMs)
        return false;
    *milliseconds = c->lastGpuMs;
    return true;
}

bool isFailed(const Context* c)
{
    return !c || c->failed;
}

void setOpticalFlowEnabled(Context* c, bool enabled)
{
    if (c && !c->inCall)
        c->opticalFlowDisabled = !enabled;
}

}  // namespace tf2fg

#if defined(TF2FG_TESTING)
// TF2FG PATCH (B-TEST v4) globals, defined in sdk_patched/src/backends/dx11/ffx_dx11.cpp (testing flavour only)
extern "C" unsigned long long tf2fgCreateCalls;
extern "C" unsigned long long tf2fgMapCalls;
extern "C" unsigned long long tf2fgFailCreateAt;
extern "C" unsigned long long tf2fgFailMapAt;
extern "C" unsigned long long tf2fgRegisterCalls;
extern "C" unsigned long long tf2fgFailRegisterAt;
extern "C" void (*tf2fgNullBindingHook)(const wchar_t* passName, const wchar_t* bindingName, int isUav);

namespace tf2fg {

unsigned long long testingBackendCreations() { return tf2fgCreateCalls; }
unsigned long long testingBackendMaps() { return tf2fgMapCalls; }
void testingFailBackendCreation(unsigned long long n) { tf2fgFailCreateAt = n ? tf2fgCreateCalls + n : 0; }
void testingFailBackendMap(unsigned long long n) { tf2fgFailMapAt = n ? tf2fgMapCalls + n : 0; }
unsigned long long testingBackendRegistrations() { return tf2fgRegisterCalls; }
void testingFailBackendRegistration(unsigned long long n) { tf2fgFailRegisterAt = n ? tf2fgRegisterCalls + n : 0; }
bool testingFaultPending() { return tf2fgFailCreateAt != 0 || tf2fgFailMapAt != 0 || tf2fgFailRegisterAt != 0; }
void testingSetNullBindingHook(TestingNullBindingFn fn) { tf2fgNullBindingHook = fn; }
void testingSetOpticalFlowEnabled(Context* c, bool enabled)
{
    if (c)
        c->opticalFlowDisabled = !enabled;
}
bool testingLastDispatchReset(const Context* c) { return c && c->lastDispatchReset; }
bool testingPrepared(const Context* c) { return c && c->prepared; }

}  // namespace tf2fg
#endif  // TF2FG_TESTING
