// This file is part of the FidelityFX SDK.
// 
// Copyright (c) 2023 Advanced Micro Devices, Inc. All rights reserved.
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.


#define INITGUID

#include <FidelityFX/host/ffx_interface.h>
#include <FidelityFX/host/ffx_util.h>
#include <FidelityFX/host/ffx_assert.h>
#include <FidelityFX/host/backends/dx11/ffx_dx11.h>
#include <../src/backends/shared/ffx_shader_blobs.h>
#include <codecvt>  // convert string to wstring
#include <mutex>
#include <new>

extern "C" void CalculateDXBCChecksum(const DWORD* pData, DWORD dwSize, DWORD dwHash[4]);

// DX11 prototypes for functions in the backend interface
FfxUInt32 GetSDKVersionDX11(FfxInterface* backendInterface);
FfxErrorCode CreateBackendContextDX11(FfxInterface* backendInterface, FfxUInt32* effectContextId);
FfxErrorCode GetDeviceCapabilitiesDX11(FfxInterface* backendInterface, FfxDeviceCapabilities* deviceCapabilities);
FfxErrorCode DestroyBackendContextDX11(FfxInterface* backendInterface, FfxUInt32 effectContextId);
FfxErrorCode CreateResourceDX11(FfxInterface* backendInterface, const FfxCreateResourceDescription* desc, FfxUInt32 effectContextId, FfxResourceInternal* outTexture);
FfxErrorCode DestroyResourceDX11(FfxInterface* backendInterface, FfxResourceInternal resource, FfxUInt32 effectContextId);
FfxErrorCode RegisterResourceDX11(FfxInterface* backendInterface, const FfxResource* inResource, FfxUInt32 effectContextId, FfxResourceInternal* outResourceInternal);
FfxResource GetResourceDX11(FfxInterface* backendInterface, FfxResourceInternal resource);
FfxErrorCode UnregisterResourcesDX11(FfxInterface* backendInterface, FfxCommandList commandList, FfxUInt32 effectContextId);
FfxResourceDescription GetResourceDescriptorDX11(FfxInterface* backendInterface, FfxResourceInternal resource);
FfxErrorCode CreatePipelineDX11(FfxInterface* backendInterface, FfxEffect effect, FfxPass passId, uint32_t permutationOptions, const FfxPipelineDescription*  desc, FfxUInt32 effectContextId, FfxPipelineState* outPass);
FfxErrorCode DestroyPipelineDX11(FfxInterface* backendInterface, FfxPipelineState* pipeline, FfxUInt32 effectContextId);
FfxErrorCode ScheduleGpuJobDX11(FfxInterface* backendInterface, const FfxGpuJobDescription* job);
FfxErrorCode ExecuteGpuJobsDX11(FfxInterface* backendInterface, FfxCommandList commandList);

#define FFX_MAX_RESOURCE_IDENTIFIER_COUNT   (128)

typedef struct PipelineState_DX11 {
    ID3D11ComputeShader* computeShader = nullptr;
    ID3D11SamplerState*  samplerStates[FFX_MAX_SAMPLERS] = {};
    uint32_t             samplerCount = 0;
} PipelineState_DX11;


typedef struct BackendContext_DX11 {

    // store for resources and resourceViews
    typedef struct Resource
    {
#ifdef _DEBUG
        wchar_t                     resourceName[64] = {};
#endif
        ID3D11Resource*             resourcePtr;
        FfxResourceDescription      resourceDescription;
        ID3D11ShaderResourceView*   srvPtr;
        ID3D11UnorderedAccessView*  uavPtr[16];
    } Resource;

    uint32_t refCount;
    uint32_t maxEffectContexts;

    ID3D11Device*           device = nullptr;
    ID3D11DeviceContext*    deviceContext = nullptr;
    ID3D11DeviceContext1*   deviceContext1 = nullptr;

    FfxGpuJobDescription*   pGpuJobs;
    uint32_t                gpuJobCount;
    bool                    gpuJobOverflow;     // TF2FG PATCH (B-JOBS): a job was dropped; the next Execute refuses the batch

    void*                   constantBufferMem[FFX_MAX_NUM_CONST_BUFFERS];
    ID3D11Buffer*           constantBufferResource[FFX_MAX_NUM_CONST_BUFFERS];
    uint32_t                constantBufferSize[FFX_MAX_NUM_CONST_BUFFERS];
    uint32_t                constantBufferOffset[FFX_MAX_NUM_CONST_BUFFERS];
    std::mutex              constantBufferMutex;

    typedef struct alignas(32) EffectContext {

        // Resource allocation
        uint32_t            nextStaticResource;
        uint32_t            nextDynamicResource;

        // Usage
        bool                active;

    } EffectContext;

    // Resource holder
    Resource*               pResources;
    EffectContext*          pEffectContexts;

} BackendContext_DX11;

FFX_API size_t ffxGetScratchMemorySizeDX11(size_t maxContexts)
{
    uint32_t resourceArraySize   = FFX_ALIGN_UP(maxContexts * FFX_MAX_RESOURCE_COUNT * sizeof(BackendContext_DX11::Resource), sizeof(uint64_t));
    uint32_t contextArraySize    = FFX_ALIGN_UP(maxContexts * sizeof(BackendContext_DX11::EffectContext), sizeof(uint32_t));
    uint32_t gpuJobDescArraySize = FFX_ALIGN_UP(maxContexts * FFX_MAX_GPU_JOBS * sizeof(FfxGpuJobDescription), sizeof(uint32_t));

    return FFX_ALIGN_UP(sizeof(BackendContext_DX11) + resourceArraySize + contextArraySize + gpuJobDescArraySize, sizeof(uint64_t));
}

// Create a FfxDevice from a ID3D11Device*
FfxDevice ffxGetDeviceDX11_Fsr31(ID3D11Device* dx11Device)
{
    FFX_ASSERT(NULL != dx11Device);
    return reinterpret_cast<FfxDevice>(dx11Device);
}

// populate interface with DX11 pointers.
FfxErrorCode ffxGetInterfaceDX11(
    FfxInterface* backendInterface,
    FfxDevice device,
    void* scratchBuffer,
    size_t scratchBufferSize,
    uint32_t maxContexts) {

    FFX_RETURN_ON_ERROR(
        backendInterface,
        FFX_ERROR_INVALID_POINTER);
    FFX_RETURN_ON_ERROR(
        scratchBuffer,
        FFX_ERROR_INVALID_POINTER);
    FFX_RETURN_ON_ERROR(
        scratchBufferSize >= ffxGetScratchMemorySizeDX11(maxContexts),
        FFX_ERROR_INSUFFICIENT_MEMORY);

    backendInterface->fpGetSDKVersion = GetSDKVersionDX11;
    backendInterface->fpCreateBackendContext = CreateBackendContextDX11;
    backendInterface->fpGetDeviceCapabilities = GetDeviceCapabilitiesDX11;
    backendInterface->fpDestroyBackendContext = DestroyBackendContextDX11;
    backendInterface->fpCreateResource = CreateResourceDX11;
    backendInterface->fpDestroyResource = DestroyResourceDX11;
    backendInterface->fpGetResource = GetResourceDX11;
    backendInterface->fpRegisterResource = RegisterResourceDX11;
    backendInterface->fpUnregisterResources = UnregisterResourcesDX11;
    backendInterface->fpGetResourceDescription = GetResourceDescriptorDX11;
    backendInterface->fpCreatePipeline = CreatePipelineDX11;
    backendInterface->fpGetPermutationBlobByIndex = ffxGetPermutationBlobByIndex;
    backendInterface->fpDestroyPipeline = DestroyPipelineDX11;
    backendInterface->fpScheduleGpuJob = ScheduleGpuJobDX11;
    backendInterface->fpExecuteGpuJobs = ExecuteGpuJobsDX11;
    backendInterface->fpSwapChainConfigureFrameGeneration = [](FfxFrameGenerationConfig const*) -> FfxErrorCode { return FFX_OK; };

    // Memory assignments
    backendInterface->scratchBuffer = scratchBuffer;
    backendInterface->scratchBufferSize = scratchBufferSize;

    BackendContext_DX11* backendContext = (BackendContext_DX11*)backendInterface->scratchBuffer;

    FFX_RETURN_ON_ERROR(
        !backendContext->refCount,
        FFX_ERROR_BACKEND_API_ERROR);

    // Clear everything out
    memset(backendContext, 0, sizeof(*backendContext));

    // Set the device
    backendInterface->device = device;

    // Assign the max number of contexts we'll be using
    backendContext->maxEffectContexts = maxContexts;

    return FFX_OK;
}

FfxCommandList ffxGetCommandListDX11(ID3D11DeviceContext* deviceContext)
{
    FFX_ASSERT(NULL != deviceContext);
    return reinterpret_cast<FfxCommandList>(deviceContext);
}

// register a DX11 resource to the backend
FfxResource ffxGetResourceDX11_Fsr31(ID3D11Resource* dx11Resource,
    FfxResourceDescription                     ffxResDescription,
    wchar_t const*                             ffxResName,
    FfxResourceStates                          state /*=FFX_RESOURCE_STATE_COMPUTE_READ*/)
{
    FfxResource resource = {};
    resource.resource    = reinterpret_cast<void*>(const_cast<ID3D11Resource*>(dx11Resource));
    resource.state = state;
    resource.description = ffxResDescription;

#ifdef _DEBUG
    if (ffxResName) {
        wcscpy_s(resource.name, ffxResName);
    }
#endif

    return resource;
}

static void SetNameDX11(ID3D11DeviceChild* resource, wchar_t const* name)
{
    if (resource)
    {
        resource->SetPrivateData(WKPDID_D3DDebugObjectNameW, static_cast<UINT>(wcslen(name) * sizeof(wchar_t)), name);
    }
}

// TF2FG PATCH (B-TIF): no MessageBox, no FormatMessage/wcstombs_s (wcstombs_s can hit the CRT invalid-parameter
// handler = __fastfail inside the game). Throw the HRESULT as a C++ exception; tf2fg_fsr.cpp catches (...) at every
// API boundary. C++ exceptions are not fatal under Northstar's VEH.
// TF2FG PATCH (B-TEST v4): instrumentation for test/tf2fg_test.cpp only, compiled ONLY into the testing flavour
// (lib/build_testing/tf2fg_fsr_testing.lib, /DTF2FG_TESTING). The production library (lib/build/tf2fg_fsr.lib) has
// no counters, no fault injection and no hooks: there tf2fgInjectedFault*() are constant false and TIF only checks
// the HRESULT.
//   tf2fgCreateCalls / tf2fgMapCalls : D3D11 object creations / constant-buffer Map calls checked by TIF (64-bit).
//   tf2fgFailCreateAt / tf2fgFailMapAt: when non-zero, the creation / Map with that ordinal is SKIPPED and reported as
//                                      E_OUTOFMEMORY (fault injection that behaves like a real failure), then reset.
//   tf2fgRegisterCalls / tf2fgFailRegisterAt: non-null RegisterResourceDX11 calls / make that one return
//                                      FFX_ERROR_OUT_OF_RANGE with internalIndex 0 (the slot-exhaustion error path).
//   tf2fgNullBindingHook              : called for every SRV/UAV that executeGpuJobCompute binds as NULL
//                                      (pass name from the shader's debug name, binding name from reflection).
#if defined(TF2FG_TESTING)
extern "C" {
unsigned long long tf2fgCreateCalls = 0;
unsigned long long tf2fgMapCalls = 0;
unsigned long long tf2fgFailCreateAt = 0;
unsigned long long tf2fgFailMapAt = 0;
unsigned long long tf2fgRegisterCalls = 0;    // non-null RegisterResourceDX11 calls
unsigned long long tf2fgFailRegisterAt = 0;   // that registration returns FFX_ERROR_OUT_OF_RANGE (like slot exhaustion)
void (*tf2fgNullBindingHook)(const wchar_t* passName, const wchar_t* bindingName, int isUav) = nullptr;
}

static bool tf2fgInjectedFault()
{
    ++tf2fgCreateCalls;
    if (tf2fgFailCreateAt != 0 && tf2fgCreateCalls >= tf2fgFailCreateAt) {
        tf2fgFailCreateAt = 0;
        return true;
    }
    return false;
}

static bool tf2fgInjectedFaultMap()
{
    ++tf2fgMapCalls;
    if (tf2fgFailMapAt != 0 && tf2fgMapCalls >= tf2fgFailMapAt) {
        tf2fgFailMapAt = 0;
        return true;
    }
    return false;
}

static void tf2fgReportNullBinding(ID3D11ComputeShader* shader, const wchar_t* bindingName, int isUav)
{
    if (!tf2fgNullBindingHook)
        return;
    wchar_t passName[64] = {};
    UINT size = UINT(sizeof(passName) - sizeof(wchar_t));
    if (!shader || FAILED(shader->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, passName)))
        passName[0] = 0;
    passName[63] = 0;
    tf2fgNullBindingHook(passName, bindingName, isUav);
}
#else
static inline bool tf2fgInjectedFault() { return false; }
static inline bool tf2fgInjectedFaultMap() { return false; }
#endif

static void TIF(HRESULT result)
{
    if (FAILED(result)) {
        throw static_cast<long>(result);
    }
}

// TF2FG PATCH (B-SAFE): bounds helpers used by the patched functions below.
static inline bool tf2fgValidResourceIndex(const BackendContext_DX11* backendContext, int32_t index)
{
    return backendContext && backendContext->pResources && index >= 0 &&
           static_cast<uint32_t>(index) < backendContext->maxEffectContexts * FFX_MAX_RESOURCE_COUNT;
}

static inline bool tf2fgValidEffectContext(const BackendContext_DX11* backendContext, FfxUInt32 effectContextId)
{
    return backendContext && backendContext->pEffectContexts && effectContextId < backendContext->maxEffectContexts;
}

// copy a reflection name into a fixed wchar_t[64] without ever calling the CRT invalid-parameter handler
static inline void tf2fgCopyName(wchar_t (&dst)[64], const char* src)
{
    size_t i = 0;
    if (src) {
        for (; i < 63 && src[i]; ++i)
            dst[i] = static_cast<wchar_t>(static_cast<unsigned char>(src[i]));
    }
    dst[i] = 0;
}

// fix up format in case resource passed for UAV cannot be mapped
static DXGI_FORMAT convertFormatUav(DXGI_FORMAT format)
{
    switch (format) {
        // Handle Depth
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT:
            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_D16_UNORM:
            return DXGI_FORMAT_R16_UNORM;

        // Handle color: assume FLOAT for 16 and 32 bit channels, else UNORM
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R32G32B32_TYPELESS:
            return DXGI_FORMAT_R32G32B32_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R32G32_TYPELESS:
            return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R16G16_TYPELESS:
            return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
            return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
        case DXGI_FORMAT_R32_TYPELESS:
            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R8G8_TYPELESS:
            return DXGI_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R16_TYPELESS:
            return DXGI_FORMAT_R16_FLOAT;
        case DXGI_FORMAT_R8_TYPELESS:
            return DXGI_FORMAT_R8_UNORM;
        default:
            return format;
    }
}

// fix up format in case resource passed for SRV cannot be mapped
static DXGI_FORMAT convertFormatSrv(DXGI_FORMAT format)
{
    switch (format) {
        // Handle Depth
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;

        // Handle Color
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;

        // Colors can map as is
    default:
        return format;
    }
}

DXGI_FORMAT ffxGetDX11FormatFromSurfaceFormat_Fsr31(FfxSurfaceFormat surfaceFormat)
{
    switch (surfaceFormat) {

        case (FFX_SURFACE_FORMAT_R32G32B32A32_TYPELESS):
            return DXGI_FORMAT_R32G32B32A32_TYPELESS;
        case (FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT):
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case (FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT):
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case (FFX_SURFACE_FORMAT_R32G32_FLOAT):
            return DXGI_FORMAT_R32G32_FLOAT;
        case (FFX_SURFACE_FORMAT_R32_UINT):
            return DXGI_FORMAT_R32_UINT;
        case(FFX_SURFACE_FORMAT_R10G10B10A2_UNORM):
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case (FFX_SURFACE_FORMAT_R8G8B8A8_TYPELESS):
            return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case (FFX_SURFACE_FORMAT_R8G8B8A8_UNORM):
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case (FFX_SURFACE_FORMAT_R8G8B8A8_SRGB):
            return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case (FFX_SURFACE_FORMAT_R8G8B8A8_SNORM):
            return DXGI_FORMAT_R8G8B8A8_SNORM;
        case (FFX_SURFACE_FORMAT_R11G11B10_FLOAT):
            return DXGI_FORMAT_R11G11B10_FLOAT;
        case (FFX_SURFACE_FORMAT_R16G16_FLOAT):
            return DXGI_FORMAT_R16G16_FLOAT;
        case (FFX_SURFACE_FORMAT_R16G16_UINT):
            return DXGI_FORMAT_R16G16_UINT;
        case (FFX_SURFACE_FORMAT_R16G16_SINT):
            return DXGI_FORMAT_R16G16_SINT;
        case (FFX_SURFACE_FORMAT_R16_FLOAT):
            return DXGI_FORMAT_R16_FLOAT;
        case (FFX_SURFACE_FORMAT_R16_UINT):
            return DXGI_FORMAT_R16_UINT;
        case (FFX_SURFACE_FORMAT_R16_UNORM):
            return DXGI_FORMAT_R16_UNORM;
        case (FFX_SURFACE_FORMAT_R16_SNORM):
            return DXGI_FORMAT_R16_SNORM;
        case (FFX_SURFACE_FORMAT_R8_UNORM):
            return DXGI_FORMAT_R8_UNORM;
        case (FFX_SURFACE_FORMAT_R8_UINT):
            return DXGI_FORMAT_R8_UINT;
        case (FFX_SURFACE_FORMAT_R8G8_UINT):
            return DXGI_FORMAT_R8G8_UINT;
        case (FFX_SURFACE_FORMAT_R8G8_UNORM):
            return DXGI_FORMAT_R8G8_UNORM;
        case (FFX_SURFACE_FORMAT_R32_FLOAT):
            return DXGI_FORMAT_R32_FLOAT;
        case (FFX_SURFACE_FORMAT_UNKNOWN):
            return DXGI_FORMAT_UNKNOWN;

        default:
            FFX_ASSERT_MESSAGE(false, "Format not yet supported");
            return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_FORMAT patchDxgiFormatWithFfxUsage(DXGI_FORMAT dxResFmt, FfxSurfaceFormat ffxFmt)
{
    DXGI_FORMAT fromFfx = ffxGetDX11FormatFromSurfaceFormat_Fsr31(ffxFmt);
    DXGI_FORMAT fmt = dxResFmt;

    switch (fmt)
    {
    // fixup typeless formats with what is passed in the ffxSurfaceFormat
    case DXGI_FORMAT_UNKNOWN:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return fromFfx;

    // fixup RGBA8 with SRGB flag passed in the ffxSurfaceFormat
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return fromFfx;
    
    // fixup depth formats as ffxGetDX12FormatFromSurfaceFormat will result in wrong format
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;

    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;

    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;

    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;

    default:
        break;
    }
    return fmt;
}

D3D11_BIND_FLAG ffxGetDX11BindFlags(FfxResourceUsage flags)
{
    int dx11ResourceFlags = D3D11_BIND_SHADER_RESOURCE;
    if (flags & FFX_RESOURCE_USAGE_RENDERTARGET) dx11ResourceFlags |= D3D11_BIND_RENDER_TARGET;
    if (flags & FFX_RESOURCE_USAGE_UAV) dx11ResourceFlags |= D3D11_BIND_UNORDERED_ACCESS;
    return D3D11_BIND_FLAG(dx11ResourceFlags);
}

FfxSurfaceFormat ffxGetSurfaceFormatDX11(DXGI_FORMAT format)
{
    switch (format) {

        case(DXGI_FORMAT_R32G32B32A32_TYPELESS):
            return FFX_SURFACE_FORMAT_R32G32B32A32_TYPELESS;
        case(DXGI_FORMAT_R32G32B32A32_FLOAT):
            return FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_UINT:
            return FFX_SURFACE_FORMAT_R32G32B32A32_UINT;
        //case DXGI_FORMAT_R32G32B32A32_SINT:
        //case DXGI_FORMAT_R32G32B32_TYPELESS:
        //case DXGI_FORMAT_R32G32B32_FLOAT:
        //case DXGI_FORMAT_R32G32B32_UINT:
        //case DXGI_FORMAT_R32G32B32_SINT:

        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case(DXGI_FORMAT_R16G16B16A16_FLOAT):
            return FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;
        //case DXGI_FORMAT_R16G16B16A16_UNORM:
        //case DXGI_FORMAT_R16G16B16A16_UINT:
        //case DXGI_FORMAT_R16G16B16A16_SNORM:
        //case DXGI_FORMAT_R16G16B16A16_SINT:

        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R32G32_FLOAT:
            return FFX_SURFACE_FORMAT_R32G32_FLOAT;
        //case DXGI_FORMAT_R32G32_FLOAT:
        //case DXGI_FORMAT_R32G32_UINT:
        //case DXGI_FORMAT_R32G32_SINT:

        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
            return FFX_SURFACE_FORMAT_R32_FLOAT;

        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
            return FFX_SURFACE_FORMAT_R32_UINT;

        case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
            return FFX_SURFACE_FORMAT_R8_UINT;

        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return FFX_SURFACE_FORMAT_R10G10B10A2_UNORM;
        //case DXGI_FORMAT_R10G10B10A2_UINT:
        
        case (DXGI_FORMAT_R11G11B10_FLOAT):
            return FFX_SURFACE_FORMAT_R11G11B10_FLOAT;

        case (DXGI_FORMAT_R8G8B8A8_TYPELESS):
            return FFX_SURFACE_FORMAT_R8G8B8A8_TYPELESS;
        case (DXGI_FORMAT_R8G8B8A8_UNORM):
            return FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
        case (DXGI_FORMAT_R8G8B8A8_UNORM_SRGB):
            return FFX_SURFACE_FORMAT_R8G8B8A8_SRGB;
        //case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
            return FFX_SURFACE_FORMAT_R8G8B8A8_SNORM;

        case DXGI_FORMAT_R16G16_TYPELESS:
        case (DXGI_FORMAT_R16G16_FLOAT):
            return FFX_SURFACE_FORMAT_R16G16_FLOAT;
        //case DXGI_FORMAT_R16G16_UNORM:
        case (DXGI_FORMAT_R16G16_UINT):
            return FFX_SURFACE_FORMAT_R16G16_UINT;
        //case DXGI_FORMAT_R16G16_SNORM
        //case DXGI_FORMAT_R16G16_SINT 

        //case DXGI_FORMAT_R32_SINT:
        case DXGI_FORMAT_R32_UINT:
            return FFX_SURFACE_FORMAT_R32_UINT;
        case DXGI_FORMAT_R32_TYPELESS:
        case(DXGI_FORMAT_D32_FLOAT):
        case(DXGI_FORMAT_R32_FLOAT):
            return FFX_SURFACE_FORMAT_R32_FLOAT;

        case DXGI_FORMAT_R8G8_TYPELESS:
        case (DXGI_FORMAT_R8G8_UINT):
            return FFX_SURFACE_FORMAT_R8G8_UINT;
        //case DXGI_FORMAT_R8G8_UNORM:
        //case DXGI_FORMAT_R8G8_SNORM:
        //case DXGI_FORMAT_R8G8_SINT:

        case DXGI_FORMAT_R16_TYPELESS:
        case (DXGI_FORMAT_R16_FLOAT):
            return FFX_SURFACE_FORMAT_R16_FLOAT;
        case (DXGI_FORMAT_R16_UINT):
            return FFX_SURFACE_FORMAT_R16_UINT;
        case DXGI_FORMAT_D16_UNORM:
        case (DXGI_FORMAT_R16_UNORM):
            return FFX_SURFACE_FORMAT_R16_UNORM;
        case (DXGI_FORMAT_R16_SNORM):
            return FFX_SURFACE_FORMAT_R16_SNORM;
        //case DXGI_FORMAT_R16_SINT:

        case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_A8_UNORM:
            return FFX_SURFACE_FORMAT_R8_UNORM;
        case DXGI_FORMAT_R8_UINT:
            return FFX_SURFACE_FORMAT_R8_UINT;
        //case DXGI_FORMAT_R8_SNORM:
        //case DXGI_FORMAT_R8_SINT:
        //case DXGI_FORMAT_R1_UNORM:

        case(DXGI_FORMAT_UNKNOWN):
            return FFX_SURFACE_FORMAT_UNKNOWN;
        default:
            FFX_ASSERT_MESSAGE(false, "Format not yet supported");
            return FFX_SURFACE_FORMAT_UNKNOWN;
    }
}

bool IsDepthDX11(DXGI_FORMAT format)
{
    return (format == DXGI_FORMAT_D16_UNORM) || 
           (format == DXGI_FORMAT_D32_FLOAT) || 
           (format == DXGI_FORMAT_D24_UNORM_S8_UINT) ||
           (format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
}

FfxResourceDescription GetFfxResourceDescriptionDX11(ID3D11Resource* pResource)
{
    FfxResourceDescription resourceDescription = {};

    // This is valid
    if (!pResource)
        return resourceDescription;

    if (pResource)
    {
        D3D11_RESOURCE_DIMENSION dimension = {};
        pResource->GetType(&dimension);
        
        if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER)
        {
            D3D11_BUFFER_DESC desc = {};
            reinterpret_cast<ID3D11Buffer*>(pResource)->GetDesc(&desc);

            resourceDescription.flags  = FFX_RESOURCE_FLAGS_NONE;
            resourceDescription.usage  = FFX_RESOURCE_USAGE_UAV;
            resourceDescription.width  = desc.ByteWidth;
            resourceDescription.height = 1;
            resourceDescription.format = ffxGetSurfaceFormatDX11(DXGI_FORMAT_UNKNOWN);

            // What should we initialize this to?? No case for this yet
            resourceDescription.depth    = 0;
            resourceDescription.mipCount = 0;

            // Set the type
            resourceDescription.type = FFX_RESOURCE_TYPE_BUFFER;
        }
        else
        {
            // Set flags properly for resource registration
            resourceDescription.flags = FFX_RESOURCE_FLAGS_NONE;

            switch (dimension)
            {
            case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
            {
                D3D11_TEXTURE1D_DESC desc = {};
                reinterpret_cast<ID3D11Texture1D*>(pResource)->GetDesc(&desc);

                resourceDescription.type = FFX_RESOURCE_TYPE_TEXTURE1D;
                resourceDescription.usage = IsDepthDX11(desc.Format) ? FFX_RESOURCE_USAGE_DEPTHTARGET : FFX_RESOURCE_USAGE_READ_ONLY;
                if ((desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) == D3D11_BIND_UNORDERED_ACCESS)
                    resourceDescription.usage = (FfxResourceUsage)(resourceDescription.usage | FFX_RESOURCE_USAGE_UAV);

                resourceDescription.width = desc.Width;
                resourceDescription.height = 1;
                resourceDescription.depth = 1;
                resourceDescription.mipCount = desc.MipLevels;
                resourceDescription.format = ffxGetSurfaceFormatDX11(desc.Format);
                break;
            }
            case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
            {
                D3D11_TEXTURE2D_DESC desc = {};
                reinterpret_cast<ID3D11Texture2D*>(pResource)->GetDesc(&desc);

                if (desc.ArraySize == 1)
                    resourceDescription.type = FFX_RESOURCE_TYPE_TEXTURE2D;
                else if (desc.ArraySize == 6)
                    resourceDescription.type = FFX_RESOURCE_TYPE_TEXTURE_CUBE;
                else
                    resourceDescription.type = FFX_RESOURCE_TYPE_TEXTURE2D;
                resourceDescription.usage = IsDepthDX11(desc.Format) ? FFX_RESOURCE_USAGE_DEPTHTARGET : FFX_RESOURCE_USAGE_READ_ONLY;
                if ((desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) == D3D11_BIND_UNORDERED_ACCESS)
                    resourceDescription.usage = (FfxResourceUsage)(resourceDescription.usage | FFX_RESOURCE_USAGE_UAV);

                resourceDescription.width = desc.Width;
                resourceDescription.height = desc.Height;
                resourceDescription.depth = desc.ArraySize;
                resourceDescription.mipCount = desc.MipLevels;
                resourceDescription.format = ffxGetSurfaceFormatDX11(desc.Format);
                break;
            }
            case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
            {
                D3D11_TEXTURE3D_DESC desc = {};
                reinterpret_cast<ID3D11Texture3D*>(pResource)->GetDesc(&desc);

                resourceDescription.type = FFX_RESOURCE_TYPE_TEXTURE3D;
                resourceDescription.usage = IsDepthDX11(desc.Format) ? FFX_RESOURCE_USAGE_DEPTHTARGET : FFX_RESOURCE_USAGE_READ_ONLY;
                if ((desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) == D3D11_BIND_UNORDERED_ACCESS)
                    resourceDescription.usage = (FfxResourceUsage)(resourceDescription.usage | FFX_RESOURCE_USAGE_UAV);

                resourceDescription.width = desc.Width;
                resourceDescription.height = desc.Height;
                resourceDescription.depth = desc.Depth;
                resourceDescription.mipCount = desc.MipLevels;
                resourceDescription.format = ffxGetSurfaceFormatDX11(desc.Format);
                break;
            }
            default:
                FFX_ASSERT_MESSAGE(false, "FFXInterface: Cauldron: Unsupported texture dimension requested. Please implement.");
                break;
            }
        }
    }

    return resourceDescription;
}

ID3D11Resource* getDX11ResourcePtr_Fsr31(BackendContext_DX11* backendContext, int32_t resourceIndex)
{
    FFX_ASSERT(NULL != backendContext);
    return reinterpret_cast<ID3D11Resource*>(backendContext->pResources[resourceIndex].resourcePtr);
}

//////////////////////////////////////////////////////////////////////////
// DX11 back end implementation

FfxUInt32 GetSDKVersionDX11(FfxInterface* backendInterface)
{
    return FFX_SDK_MAKE_VERSION(FFX_SDK_VERSION_MAJOR, FFX_SDK_VERSION_MINOR, FFX_SDK_VERSION_PATCH);
}

// initialize the DX11 backend
FfxErrorCode CreateBackendContextDX11(FfxInterface* backendInterface, FfxUInt32* effectContextId)
{
    FFX_ASSERT(NULL != backendInterface);
    FFX_ASSERT(NULL != backendInterface->device);

    HRESULT result = S_OK;
    ID3D11Device* dx11Device = reinterpret_cast<ID3D11Device*>(backendInterface->device);

    // set up some internal resources we need (space for resource views and constant buffers)
    BackendContext_DX11* backendContext = (BackendContext_DX11*)backendInterface->scratchBuffer;

    // TF2FG PATCH (B-CTX): validate everything we are about to dereference.
    if (!backendContext || !effectContextId || dx11Device == NULL)
        return FFX_ERROR_INVALID_POINTER;

    // Set things up if this is the first invocation
    if (!backendContext->refCount) {

        new (&backendContext->constantBufferMutex) std::mutex();

        if (dx11Device != NULL) {

            dx11Device->AddRef();
            backendContext->device = dx11Device;

            dx11Device->GetImmediateContext(&backendContext->deviceContext);
            if (backendContext->deviceContext == NULL) {
                dx11Device->Release();
                backendContext->device = NULL;
                return FFX_ERROR_BACKEND_API_ERROR;
            }
            if (FAILED(backendContext->deviceContext->QueryInterface(IID_PPV_ARGS(&backendContext->deviceContext1))))
                backendContext->deviceContext1 = NULL;
        }

        // Map all of our pointers
        uint32_t gpuJobDescArraySize = FFX_ALIGN_UP(backendContext->maxEffectContexts * FFX_MAX_GPU_JOBS * sizeof(FfxGpuJobDescription), sizeof(uint32_t));
        uint32_t resourceArraySize = FFX_ALIGN_UP(backendContext->maxEffectContexts * FFX_MAX_RESOURCE_COUNT * sizeof(BackendContext_DX11::Resource), sizeof(uint64_t));
        uint32_t contextArraySize = FFX_ALIGN_UP(backendContext->maxEffectContexts * sizeof(BackendContext_DX11::EffectContext), sizeof(uint32_t));
        uint8_t* pMem = (uint8_t*)((BackendContext_DX11*)(backendContext + 1));

        // Map gpu job array
        backendContext->pGpuJobs = (FfxGpuJobDescription*)pMem;
        memset(backendContext->pGpuJobs, 0, gpuJobDescArraySize);
        pMem += gpuJobDescArraySize;

        // Map the resources
        backendContext->pResources = (BackendContext_DX11::Resource*)(pMem);
        memset(backendContext->pResources, 0, resourceArraySize);
        pMem += resourceArraySize;

        // Map the effect contexts
        backendContext->pEffectContexts = reinterpret_cast<BackendContext_DX11::EffectContext*>(pMem);
        memset(backendContext->pEffectContexts, 0, contextArraySize);
    }

    // TF2FG PATCH (B-CTX): the original returned FFX_OK with *effectContextId uninitialised when every slot was
    // taken (silent memory corruption). Find the slot first; only then take a reference.
    for (uint32_t i = 0; i < backendContext->maxEffectContexts; ++i) {
        if (!backendContext->pEffectContexts[i].active) {
            *effectContextId = i;

            // Reset everything accordingly
            BackendContext_DX11::EffectContext& effectContext = backendContext->pEffectContexts[i];
            effectContext.active = true;
            effectContext.nextStaticResource = (i * FFX_MAX_RESOURCE_COUNT) + 1;
            effectContext.nextDynamicResource = (i * FFX_MAX_RESOURCE_COUNT) + FFX_MAX_RESOURCE_COUNT - 1;

            // Increment the ref count
            ++backendContext->refCount;
            return FFX_OK;
        }
    }

    // no free effect context: undo the first-invocation setup if nobody else holds a reference
    if (!backendContext->refCount) {
        if (backendContext->deviceContext1) { backendContext->deviceContext1->Release(); backendContext->deviceContext1 = NULL; }
        if (backendContext->deviceContext)  { backendContext->deviceContext->Release();  backendContext->deviceContext = NULL; }
        if (backendContext->device)         { backendContext->device->Release();         backendContext->device = NULL; }
    }
    return FFX_ERROR_OUT_OF_RANGE;
}

// query device capabilities to select the optimal shader permutation
FfxErrorCode GetDeviceCapabilitiesDX11(FfxInterface* backendInterface, FfxDeviceCapabilities* deviceCapabilities)
{
    FFX_ASSERT(NULL != backendInterface);
    FFX_ASSERT(NULL != backendInterface->device);
    FFX_ASSERT(NULL != deviceCapabilities);
    ID3D11Device* dx11Device = reinterpret_cast<ID3D11Device*>(backendInterface->device);

    // TF2FG PATCH (B-CAPS): the components pass an uninitialised local here and read waveLaneCountMin/Max, which
    // the original never set. Zero the struct first; no wave ops and no FP16 permutations exist in our build.
    if (!backendInterface || !deviceCapabilities || !dx11Device)
        return FFX_ERROR_INVALID_POINTER;
    memset(deviceCapabilities, 0, sizeof(*deviceCapabilities));

    // Check if we have shader model 6.6
    D3D_FEATURE_LEVEL shaderModel = dx11Device->GetFeatureLevel();
    switch (shaderModel) {

    case D3D_FEATURE_LEVEL_10_0:
    case D3D_FEATURE_LEVEL_10_1:
    case D3D_FEATURE_LEVEL_11_0:
    case D3D_FEATURE_LEVEL_11_1:
        deviceCapabilities->minimumSupportedShaderModel = FFX_SHADER_MODEL_5_1;
        break;

    case D3D_FEATURE_LEVEL_12_0:
        deviceCapabilities->minimumSupportedShaderModel = FFX_SHADER_MODEL_6_0;
        break;

    case D3D_FEATURE_LEVEL_12_1:
        deviceCapabilities->minimumSupportedShaderModel = FFX_SHADER_MODEL_6_3;
        break;

    case D3D_FEATURE_LEVEL_12_2:
        deviceCapabilities->minimumSupportedShaderModel = FFX_SHADER_MODEL_6_5;
        break;

    default:
        deviceCapabilities->minimumSupportedShaderModel = FFX_SHADER_MODEL_6_6;
        break;
    }

    // TF2FG PATCH (B-CAPS): only FFX_HALF=0 blobs are built, so never report FP16 (keeps the permutation key exact).
    deviceCapabilities->fp16Supported = false;
    deviceCapabilities->waveLaneCountMin = 0;
    deviceCapabilities->waveLaneCountMax = 0;

    return FFX_OK;
}

// deinitialize the DX11 backend
FfxErrorCode DestroyBackendContextDX11(FfxInterface* backendInterface, FfxUInt32 effectContextId)
{
    FFX_ASSERT(NULL != backendInterface);
    BackendContext_DX11* backendContext = (BackendContext_DX11*)backendInterface->scratchBuffer;
    FFX_ASSERT(backendContext->refCount > 0);

    // TF2FG PATCH (B-CTX): bounds / double-destroy guard.
    if (!tf2fgValidEffectContext(backendContext, effectContextId) || backendContext->refCount == 0 ||
        !backendContext->pEffectContexts[effectContextId].active)
        return FFX_ERROR_INVALID_ARGUMENT;

    // Delete any resources allocated by this context
    BackendContext_DX11::EffectContext& effectContext = backendContext->pEffectContexts[effectContextId];
    for (uint32_t currentStaticResourceIndex = effectContextId * FFX_MAX_RESOURCE_COUNT; currentStaticResourceIndex < effectContext.nextStaticResource; ++currentStaticResourceIndex) {
        if (backendContext->pResources[currentStaticResourceIndex].resourcePtr) {
            FFX_ASSERT_MESSAGE(false, "FFXInterface: DX11: SDK Resource was not destroyed prior to destroying the backend context. There is a resource leak.");
            FfxResourceInternal internalResource = { static_cast<int32_t>(currentStaticResourceIndex) };
            DestroyResourceDX11(backendInterface, internalResource, effectContextId);
        }
    }
    for (uint32_t currentResourceIndex = effectContextId * FFX_MAX_RESOURCE_COUNT; currentResourceIndex < effectContextId * FFX_MAX_RESOURCE_COUNT + FFX_MAX_RESOURCE_COUNT; ++currentResourceIndex) {
        if (backendContext->pResources[currentResourceIndex].resourcePtr) {
            FfxResourceInternal internalResource = { static_cast<int32_t>(currentResourceIndex) };
            DestroyResourceDX11(backendInterface, internalResource, effectContextId);
        }
    }

    // Free up for use by another context
    effectContext.nextStaticResource = 0;
    effectContext.active = false;

    // Decrement ref count
    --backendContext->refCount;

    if (!backendContext->refCount) {

        // release constant buffer pool
        for (size_t i = 0; i < FFX_MAX_NUM_CONST_BUFFERS; ++i) {

            if (backendContext->constantBufferResource[i] != NULL) {
                backendContext->constantBufferResource[i]->Release();
                backendContext->constantBufferResource[i] = NULL;
                backendContext->constantBufferMem[i] = nullptr;
                backendContext->constantBufferOffset[i] = 0;
                backendContext->constantBufferSize[i] = 0;
            }
        }
        backendContext->gpuJobCount = 0;
        backendContext->gpuJobOverflow = false;

        if (backendContext->deviceContext1 != NULL) {
            backendContext->deviceContext1->Release();
            backendContext->deviceContext1 = NULL;
        }
        if (backendContext->deviceContext != NULL) {
            backendContext->deviceContext->Release();
            backendContext->deviceContext = NULL;
        }
        if (backendContext->device != NULL) {
            backendContext->device->Release();
            backendContext->device = NULL;
        }
    }

    return FFX_OK;
}

/// create a internal resource that will stay alive until effect gets shut down
// TF2FG PATCH (B-RES): rewritten with the original's structure, plus
//   - structured buffers (BUFFER + FFX_SURFACE_FORMAT_UNKNOWN + stride>0, e.g. FI_Counters) get
//     D3D11_RESOURCE_MISC_BUFFER_STRUCTURED + StructureByteStride and BOTH a UAV and an SRV (FI's setup/pyramid passes
//     write rw_counters as a UAV, the interpolation pass reads r_counters as an SRV). The original created a plain
//     buffer and only one view, with an UNKNOWN-format UAV that made TIF throw.
//   - no integer division by a possibly-zero value (stride, height, depth), bounds-checked slot allocation, the UAV
//     loop clamped to the 16-entry uavPtr array, and an SRV only when the resource is SRV-bindable.
FfxErrorCode CreateResourceDX11(
    FfxInterface* backendInterface,
    const FfxCreateResourceDescription* createResourceDescription,
    FfxUInt32 effectContextId,
    FfxResourceInternal* outTexture
)
{
    if (!backendInterface || !createResourceDescription || !outTexture)
        return FFX_ERROR_INVALID_POINTER;

    BackendContext_DX11* backendContext = (BackendContext_DX11*)backendInterface->scratchBuffer;
    if (!tf2fgValidEffectContext(backendContext, effectContextId))
        return FFX_ERROR_INVALID_ARGUMENT;
    BackendContext_DX11::EffectContext& effectContext = backendContext->pEffectContexts[effectContextId];
    ID3D11Device* dx11Device = backendContext->device;
    if (!dx11Device)
        return FFX_ERROR_NULL_DEVICE;

    if (!(effectContext.nextStaticResource + 1 < effectContext.nextDynamicResource))
        return FFX_ERROR_OUT_OF_RANGE;

    const FfxResourceDescription& ffxDesc = createResourceDescription->resourceDescription;

    // reject degenerate sizes up front (CreateTexture would fail anyway; the pitch math below would divide by 0)
    if (ffxDesc.type == FFX_RESOURCE_TYPE_BUFFER) {
        if (ffxDesc.size == 0)
            return FFX_ERROR_INVALID_ARGUMENT;
    } else {
        if (ffxDesc.width == 0 || ffxDesc.height == 0 || ffxDesc.depth == 0)
            return FFX_ERROR_INVALID_ARGUMENT;
    }

    const bool structuredBuffer = (ffxDesc.type == FFX_RESOURCE_TYPE_BUFFER) &&
                                  (ffxDesc.format == FFX_SURFACE_FORMAT_UNKNOWN) && (ffxDesc.stride > 0);
    if (ffxDesc.type == FFX_RESOURCE_TYPE_BUFFER) {
        if (ffxDesc.stride == 0 || (ffxDesc.size % ffxDesc.stride) != 0)
            return FFX_ERROR_INVALID_ARGUMENT;
    }

    outTexture->internalIndex = effectContext.nextStaticResource++;
    BackendContext_DX11::Resource* backendResource = &backendContext->pResources[outTexture->internalIndex];
    backendResource->resourceDescription = ffxDesc;

    D3D11_BUFFER_DESC dx11BufferDescription = {};
    D3D11_TEXTURE1D_DESC dx11Texture1DDescription = {};
    D3D11_TEXTURE2D_DESC dx11Texture2DDescription = {};
    D3D11_TEXTURE3D_DESC dx11Texture3DDescription = {};

    switch (ffxDesc.type) {

    case FFX_RESOURCE_TYPE_BUFFER:
        dx11BufferDescription.ByteWidth = ffxDesc.width;
        dx11BufferDescription.Usage = D3D11_USAGE_DEFAULT;
        dx11BufferDescription.BindFlags = ffxGetDX11BindFlags(backendResource->resourceDescription.usage);
        if (structuredBuffer) {
            dx11BufferDescription.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            dx11BufferDescription.StructureByteStride = ffxDesc.stride;
        }
        break;

    case FFX_RESOURCE_TYPE_TEXTURE1D:
        dx11Texture1DDescription.Format = ffxGetDX11FormatFromSurfaceFormat_Fsr31(ffxDesc.format);
        dx11Texture1DDescription.Width = ffxDesc.width;
        dx11Texture1DDescription.ArraySize = ffxDesc.depth;
        dx11Texture1DDescription.MipLevels = ffxDesc.mipCount;
        dx11Texture1DDescription.Usage = D3D11_USAGE_DEFAULT;
        dx11Texture1DDescription.BindFlags = ffxGetDX11BindFlags(backendResource->resourceDescription.usage);
        break;

    case FFX_RESOURCE_TYPE_TEXTURE_CUBE:
        dx11Texture2DDescription.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    case FFX_RESOURCE_TYPE_TEXTURE2D:
        dx11Texture2DDescription.Format = ffxGetDX11FormatFromSurfaceFormat_Fsr31(ffxDesc.format);
        dx11Texture2DDescription.Width = ffxDesc.width;
        dx11Texture2DDescription.Height = ffxDesc.height;
        dx11Texture2DDescription.ArraySize = ffxDesc.depth;
        dx11Texture2DDescription.MipLevels = ffxDesc.mipCount;
        dx11Texture2DDescription.Usage = D3D11_USAGE_DEFAULT;
        dx11Texture2DDescription.BindFlags = ffxGetDX11BindFlags(backendResource->resourceDescription.usage);
        dx11Texture2DDescription.SampleDesc.Count = 1;
        break;

    case FFX_RESOURCE_TYPE_TEXTURE3D:
        dx11Texture3DDescription.Format = ffxGetDX11FormatFromSurfaceFormat_Fsr31(ffxDesc.format);
        dx11Texture3DDescription.Width = ffxDesc.width;
        dx11Texture3DDescription.Height = ffxDesc.height;
        dx11Texture3DDescription.Depth = ffxDesc.depth;
        dx11Texture3DDescription.MipLevels = ffxDesc.mipCount;
        dx11Texture3DDescription.Usage = D3D11_USAGE_DEFAULT;
        dx11Texture3DDescription.BindFlags = ffxGetDX11BindFlags(backendResource->resourceDescription.usage);
        break;

    default:
        return FFX_ERROR_INVALID_ARGUMENT;
    }

    ID3D11Resource* dx11Resource = nullptr;
    if (createResourceDescription->heapType == FFX_HEAP_TYPE_UPLOAD) {

        return FFX_OK;

    }
    else {

        D3D11_SUBRESOURCE_DATA dx11SubResourceData = {};
        D3D11_SUBRESOURCE_DATA* pSubResourceData = nullptr;
        if (createResourceDescription->initData && createResourceDescription->initDataSize) {
            pSubResourceData = &dx11SubResourceData;
            pSubResourceData->pSysMem = createResourceDescription->initData;
            pSubResourceData->SysMemPitch = createResourceDescription->initDataSize;
            pSubResourceData->SysMemSlicePitch = createResourceDescription->initDataSize;
        }

        switch (ffxDesc.type) {

        case FFX_RESOURCE_TYPE_BUFFER:
            TIF(tf2fgInjectedFault() ? E_OUTOFMEMORY : dx11Device->CreateBuffer(&dx11BufferDescription, pSubResourceData, (ID3D11Buffer**)&dx11Resource));
            break;

        case FFX_RESOURCE_TYPE_TEXTURE1D:
            TIF(tf2fgInjectedFault() ? E_OUTOFMEMORY : dx11Device->CreateTexture1D(&dx11Texture1DDescription, pSubResourceData, (ID3D11Texture1D**)&dx11Resource));
            break;

        case FFX_RESOURCE_TYPE_TEXTURE_CUBE:
        case FFX_RESOURCE_TYPE_TEXTURE2D:
            if (pSubResourceData)   // Height validated non-zero above
                dx11SubResourceData.SysMemPitch /= dx11Texture2DDescription.Height;
            TIF(tf2fgInjectedFault() ? E_OUTOFMEMORY : dx11Device->CreateTexture2D(&dx11Texture2DDescription, pSubResourceData, (ID3D11Texture2D**)&dx11Resource));
            break;

        case FFX_RESOURCE_TYPE_TEXTURE3D:
            if (pSubResourceData) { // Height/Depth validated non-zero above
                dx11SubResourceData.SysMemPitch /= dx11Texture3DDescription.Height;
                dx11SubResourceData.SysMemPitch /= dx11Texture3DDescription.Depth;
                dx11SubResourceData.SysMemSlicePitch /= dx11Texture3DDescription.Depth;
            }
            TIF(tf2fgInjectedFault() ? E_OUTOFMEMORY : dx11Device->CreateTexture3D(&dx11Texture3DDescription, pSubResourceData, (ID3D11Texture3D**)&dx11Resource));
            break;

        default:
            break;
        }

        if (!dx11Resource)
            return FFX_ERROR_BACKEND_API_ERROR;

        if (createResourceDescription->name)
            SetNameDX11(dx11Resource, createResourceDescription->name);
        backendResource->resourcePtr = dx11Resource;

#ifdef _DEBUG
        wcscpy_s(backendResource->resourceName, createResourceDescription->name);
#endif

        // Create SRVs and UAVs
        {
            D3D11_UNORDERED_ACCESS_VIEW_DESC dx11UavDescription = {};
            D3D11_SHADER_RESOURCE_VIEW_DESC dx11SrvDescription = {};

            // we still want to respect the format provided in the description for SRGB or TYPELESS resources
            DXGI_FORMAT descFormat = {};

            bool requestArrayView = FFX_CONTAINS_FLAG(backendResource->resourceDescription.usage, FFX_RESOURCE_USAGE_ARRAYVIEW);

            D3D11_RESOURCE_DIMENSION resourceDimension = D3D11_RESOURCE_DIMENSION(0);
            dx11Resource->GetType(&resourceDimension);

            UINT bindFlags = 0;

            switch (resourceDimension) {

            case D3D11_RESOURCE_DIMENSION_BUFFER:
                reinterpret_cast<ID3D11Buffer*>(dx11Resource)->GetDesc(&dx11BufferDescription);
                bindFlags = dx11BufferDescription.BindFlags;
                if (structuredBuffer) {
                    dx11UavDescription.Format = DXGI_FORMAT_UNKNOWN;
                    dx11SrvDescription.Format = DXGI_FORMAT_UNKNOWN;
                } else {
                    descFormat = patchDxgiFormatWithFfxUsage(DXGI_FORMAT_UNKNOWN, backendResource->resourceDescription.format);
                    dx11UavDescription.Format = convertFormatUav(descFormat);
                    dx11SrvDescription.Format = convertFormatUav(descFormat);
                }
                dx11UavDescription.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
                dx11SrvDescription.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
                break;

            case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
                reinterpret_cast<ID3D11Texture1D*>(dx11Resource)->GetDesc(&dx11Texture1DDescription);
                bindFlags = dx11Texture1DDescription.BindFlags;
                descFormat = patchDxgiFormatWithFfxUsage(dx11Texture1DDescription.Format, backendResource->resourceDescription.format);
                dx11UavDescription.Format = convertFormatUav(descFormat);
                dx11SrvDescription.Format = convertFormatUav(descFormat);
                if (dx11Texture1DDescription.ArraySize > 1 || requestArrayView)
                {
                    dx11UavDescription.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1DARRAY;
                    dx11UavDescription.Texture1DArray.ArraySize = dx11Texture1DDescription.ArraySize;
                    dx11UavDescription.Texture1DArray.FirstArraySlice = 0;
                    dx11UavDescription.Texture1DArray.MipSlice = 0;

                    dx11SrvDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
                    dx11SrvDescription.Texture1DArray.ArraySize = dx11Texture1DDescription.ArraySize;
                    dx11SrvDescription.Texture1DArray.FirstArraySlice = 0;
                    dx11SrvDescription.Texture1DArray.MipLevels = dx11Texture1DDescription.MipLevels;
                    dx11SrvDescription.Texture1DArray.MostDetailedMip = 0;
                }
                else
                {
                    dx11UavDescription.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1D;
                    dx11UavDescription.Texture1D.MipSlice = 0;

                    dx11SrvDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D;
                    dx11SrvDescription.Texture1D.MipLevels = dx11Texture1DDescription.MipLevels;
                    dx11SrvDescription.Texture1D.MostDetailedMip = 0;
                }
                break;

            case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
                reinterpret_cast<ID3D11Texture2D*>(dx11Resource)->GetDesc(&dx11Texture2DDescription);
                bindFlags = dx11Texture2DDescription.BindFlags;
                descFormat = patchDxgiFormatWithFfxUsage(dx11Texture2DDescription.Format, backendResource->resourceDescription.format);
                dx11UavDescription.Format = convertFormatUav(descFormat);
                dx11SrvDescription.Format = convertFormatUav(descFormat);
                if (dx11Texture2DDescription.ArraySize > 1 || requestArrayView)
                {
                    dx11UavDescription.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
                    dx11UavDescription.Texture2DArray.ArraySize = dx11Texture2DDescription.ArraySize;
                    dx11UavDescription.Texture2DArray.FirstArraySlice = 0;
                    dx11UavDescription.Texture2DArray.MipSlice = 0;

                    dx11SrvDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                    dx11SrvDescription.Texture2DArray.ArraySize = dx11Texture2DDescription.ArraySize;
                    dx11SrvDescription.Texture2DArray.FirstArraySlice = 0;
                    dx11SrvDescription.Texture2DArray.MipLevels = dx11Texture2DDescription.MipLevels;
                    dx11SrvDescription.Texture2DArray.MostDetailedMip = 0;
                }
                else
                {
                    dx11UavDescription.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
                    dx11UavDescription.Texture2D.MipSlice = 0;

                    dx11SrvDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    dx11SrvDescription.Texture2D.MipLevels = dx11Texture2DDescription.MipLevels;
                    dx11SrvDescription.Texture2D.MostDetailedMip = 0;
                }
                break;

            case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
                reinterpret_cast<ID3D11Texture3D*>(dx11Resource)->GetDesc(&dx11Texture3DDescription);
                bindFlags = dx11Texture3DDescription.BindFlags;
                descFormat = patchDxgiFormatWithFfxUsage(dx11Texture3DDescription.Format, backendResource->resourceDescription.format);
                dx11UavDescription.Format = convertFormatUav(descFormat);
                dx11SrvDescription.Format = convertFormatUav(descFormat);
                dx11UavDescription.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
                dx11SrvDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
                dx11SrvDescription.Texture3D.MipLevels = dx11Texture3DDescription.MipLevels;
                dx11SrvDescription.Texture3D.MostDetailedMip = 0;
                break;

            default:
                break;
            }

            if (resourceDimension == D3D11_RESOURCE_DIMENSION_BUFFER) {

                // stride validated non-zero above
                const UINT numElements = backendResource->resourceDescription.size / backendResource->resourceDescription.stride;

                // UAV
                if (bindFlags & D3D11_BIND_UNORDERED_ACCESS) {
                    dx11UavDescription.Buffer.FirstElement = 0;
                    dx11UavDescription.Buffer.NumElements = numElements;
                    TIF(tf2fgInjectedFault() ? E_OUTOFMEMORY : dx11Device->CreateUnorderedAccessView(dx11Resource, &dx11UavDescription, &backendResource->uavPtr[0]));
                }
                // SRV (the original made this an else-branch, so a UAV buffer could never be read as an SRV)
                if (bindFlags & D3D11_BIND_SHADER_RESOURCE) {
                    dx11SrvDescription.Buffer.FirstElement = 0;
                    dx11SrvDescription.Buffer.NumElements = numElements;
                    TIF(tf2fgInjectedFault() ? E_OUTOFMEMORY : dx11Device->CreateShaderResourceView(dx11Resource, &dx11SrvDescription, &backendResource->srvPtr));
                }
            }
            else {
                // CPU readable
                if (bindFlags & D3D11_BIND_SHADER_RESOURCE)
                    TIF(tf2fgInjectedFault() ? E_OUTOFMEMORY : dx11Device->CreateShaderResourceView(dx11Resource, &dx11SrvDescription, &backendResource->srvPtr));

                // UAV
                if (bindFlags & D3D11_BIND_UNORDERED_ACCESS) {

                    int32_t mipLevels = FFX_MAXIMUM(FFX_MAXIMUM(dx11Texture1DDescription.MipLevels, dx11Texture2DDescription.MipLevels), dx11Texture3DDescription.MipLevels);
                    const int32_t uavSlots = int32_t(sizeof(backendResource->uavPtr) / sizeof(backendResource->uavPtr[0]));
                    if (mipLevels > uavSlots)
                        mipLevels = uavSlots;

                    for (int32_t currentMipIndex = 0; currentMipIndex < mipLevels; ++currentMipIndex) {

                        dx11UavDescription.Texture2D.MipSlice = currentMipIndex;

                        TIF(tf2fgInjectedFault() ? E_OUTOFMEMORY : dx11Device->CreateUnorderedAccessView(dx11Resource, &dx11UavDescription, &backendResource->uavPtr[currentMipIndex]));
                    }
                }
            }
        }
    }

    return FFX_OK;
}

FfxErrorCode DestroyResourceDX11(
    FfxInterface* backendInterface,
    FfxResourceInternal resource,
    FfxUInt32 effectContextId)
{
    FFX_ASSERT(NULL != backendInterface);

    BackendContext_DX11* backendContext = (BackendContext_DX11*)backendInterface->scratchBuffer;
    // TF2FG PATCH (B-SAFE): bounds check (the index range test below then keeps us inside pResources)
    if (!tf2fgValidEffectContext(backendContext, effectContextId) || !backendContext->pResources)
        return FFX_ERROR_INVALID_ARGUMENT;
    if ((resource.internalIndex >= int32_t(effectContextId * FFX_MAX_RESOURCE_COUNT)) && (resource.internalIndex < int32_t(effectContextId * FFX_MAX_RESOURCE_COUNT + FFX_MAX_RESOURCE_COUNT))) {

        if (backendContext->pResources[resource.internalIndex].srvPtr) {
            backendContext->pResources[resource.internalIndex].srvPtr->Release();
            backendContext->pResources[resource.internalIndex].srvPtr = nullptr;
        }
        for (int32_t currentMipIndex = 0; currentMipIndex < 16; ++currentMipIndex) {
            if (backendContext->pResources[resource.internalIndex].uavPtr[currentMipIndex]) {
                backendContext->pResources[resource.internalIndex].uavPtr[currentMipIndex]->Release();
                backendContext->pResources[resource.internalIndex].uavPtr[currentMipIndex] = nullptr;
            }
        }
        if (backendContext->pResources[resource.internalIndex].resourcePtr) {
            backendContext->pResources[resource.internalIndex].resourcePtr->Release();
            backendContext->pResources[resource.internalIndex].resourcePtr = nullptr;
        }

        return FFX_OK;
    }

    return FFX_ERROR_OUT_OF_RANGE;
}

// TF2FG PATCH (B-REG): rewritten with the original's structure, plus
//   - slot exhaustion returns an error (index 0 = NULL resource) instead of overwriting static resources,
//   - UAVs are created for max(1, min(mipCount, texture MipLevels, 16)) mips: with the caller's mipCount 0 the
//     original created no UAV at all (all writes silently dropped, CLEAR jobs on a null view), and a mipCount > 16
//     overran uavPtr[16],
//   - SRV/UAV are only created when the resource has the matching bind flag, every view creation is checked,
//   - buffer views never divide by a zero stride; structured buffers get UNKNOWN-format views.
FfxErrorCode RegisterResourceDX11(
    FfxInterface* backendInterface,
    const FfxResource* inFfxResource,
    FfxUInt32 effectContextId,
    FfxResourceInternal* outFfxResourceInternal
)
{
    if (!backendInterface || !inFfxResource || !outFfxResourceInternal)
        return FFX_ERROR_INVALID_POINTER;

    BackendContext_DX11* backendContext = (BackendContext_DX11*)(backendInterface->scratchBuffer);
    outFfxResourceInternal->internalIndex = 0; // FFX_<feature>_RESOURCE_IDENTIFIER_NULL until proven otherwise
    if (!tf2fgValidEffectContext(backendContext, effectContextId))
        return FFX_ERROR_INVALID_ARGUMENT;

    ID3D11Device* dx11Device = reinterpret_cast<ID3D11Device*>(backendContext->device);
    ID3D11Resource* dx11Resource = reinterpret_cast<ID3D11Resource*>(inFfxResource->resource);
    BackendContext_DX11::EffectContext& effectContext = backendContext->pEffectContexts[effectContextId];

    if (dx11Resource == nullptr) {

        outFfxResourceInternal->internalIndex = 0; // Always maps to FFX_<feature>_RESOURCE_IDENTIFIER_NULL;
        return FFX_OK;
    }
#if defined(TF2FG_TESTING)
    ++tf2fgRegisterCalls;
    if (tf2fgFailRegisterAt != 0 && tf2fgRegisterCalls >= tf2fgFailRegisterAt) {
        tf2fgFailRegisterAt = 0;
        return FFX_ERROR_OUT_OF_RANGE;  // internalIndex stays 0, exactly like real slot exhaustion
    }
#endif
    if (!dx11Device)
        return FFX_ERROR_NULL_DEVICE;

    if (!(effectContext.nextDynamicResource > effectContext.nextStaticResource))
        return FFX_ERROR_OUT_OF_RANGE;
    const int32_t slot = int32_t(effectContext.nextDynamicResource--);
    outFfxResourceInternal->internalIndex = slot;

    BackendContext_DX11::Resource* backendResource = &backendContext->pResources[slot];

    if (backendResource->resourcePtr == dx11Resource)
    {
        return FFX_OK;
    }

    DestroyResourceDX11(backendInterface, *outFfxResourceInternal, effectContextId);

    backendResource->resourcePtr = dx11Resource;
    if (backendResource->resourcePtr)
        backendResource->resourcePtr->AddRef();

#ifdef _DEBUG
    const wchar_t* name = inFfxResource->name;
    if (name) {
        wcscpy_s(backendResource->resourceName, name);
    }
#endif

    // create resource views
    if (dx11Resource) {

        D3D11_UNORDERED_ACCESS_VIEW_DESC dx11UavDescription = {};
        D3D11_SHADER_RESOURCE_VIEW_DESC dx11SrvDescription = {};

        // we still want to respect the format provided in the description for SRGB or TYPELESS resources
        DXGI_FORMAT descFormat = {};

        bool requestArrayView = FFX_CONTAINS_FLAG(inFfxResource->description.usage, FFX_RESOURCE_USAGE_ARRAYVIEW);

        D3D11_RESOURCE_DIMENSION resourceDimension = D3D11_RESOURCE_DIMENSION(0);
        dx11Resource->GetType(&resourceDimension);

        D3D11_BUFFER_DESC dx11BufferDesc = {};
        D3D11_TEXTURE1D_DESC dx11Texture1DDesc = {};
        D3D11_TEXTURE2D_DESC dx11Texture2DDesc = {};
        D3D11_TEXTURE3D_DESC dx11Texture3DDesc = {};
        UINT bindFlags = 0;
        UINT textureMipLevels = 1;

        switch (resourceDimension) {

        case D3D11_RESOURCE_DIMENSION_BUFFER:
            reinterpret_cast<ID3D11Buffer*>(dx11Resource)->GetDesc(&dx11BufferDesc);
            bindFlags = dx11BufferDesc.BindFlags;
            if (dx11BufferDesc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) {
                dx11UavDescription.Format = DXGI_FORMAT_UNKNOWN;
                dx11SrvDescription.Format = DXGI_FORMAT_UNKNOWN;
            } else {
                descFormat = patchDxgiFormatWithFfxUsage(DXGI_FORMAT_UNKNOWN, inFfxResource->description.format);
                dx11UavDescription.Format = convertFormatUav(descFormat);
                dx11SrvDescription.Format = convertFormatSrv(descFormat);
            }
            dx11UavDescription.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            dx11SrvDescription.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            backendResource->resourceDescription.type = FFX_RESOURCE_TYPE_BUFFER;
            backendResource->resourceDescription.size = inFfxResource->description.size;
            backendResource->resourceDescription.stride = inFfxResource->description.stride;
            backendResource->resourceDescription.alignment = 0;
            break;

        case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
            reinterpret_cast<ID3D11Texture1D*>(dx11Resource)->GetDesc(&dx11Texture1DDesc);
            bindFlags = dx11Texture1DDesc.BindFlags;
            textureMipLevels = dx11Texture1DDesc.MipLevels;
            descFormat = patchDxgiFormatWithFfxUsage(dx11Texture1DDesc.Format, inFfxResource->description.format);
            dx11UavDescription.Format = convertFormatUav(descFormat);
            dx11SrvDescription.Format = convertFormatSrv(descFormat);
            if (dx11Texture1DDesc.ArraySize > 1 || requestArrayView)
            {
                dx11UavDescription.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1DARRAY;
                dx11UavDescription.Texture1DArray.ArraySize = dx11Texture1DDesc.ArraySize;
                dx11UavDescription.Texture1DArray.FirstArraySlice = 0;
                dx11UavDescription.Texture1DArray.MipSlice = 0;

                dx11SrvDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
                dx11SrvDescription.Texture1DArray.ArraySize = dx11Texture1DDesc.ArraySize;
                dx11SrvDescription.Texture1DArray.FirstArraySlice = 0;
                dx11SrvDescription.Texture1DArray.MipLevels = dx11Texture1DDesc.MipLevels;
                dx11SrvDescription.Texture1DArray.MostDetailedMip = 0;
            }
            else
            {
                dx11UavDescription.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1D;
                dx11UavDescription.Texture1D.MipSlice = 0;

                dx11SrvDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D;
                dx11SrvDescription.Texture1D.MipLevels = dx11Texture1DDesc.MipLevels;
                dx11SrvDescription.Texture1D.MostDetailedMip = 0;
            }

            backendResource->resourceDescription.type = FFX_RESOURCE_TYPE_TEXTURE1D;
            backendResource->resourceDescription.format = inFfxResource->description.format;
            backendResource->resourceDescription.width = inFfxResource->description.width;
            backendResource->resourceDescription.mipCount = inFfxResource->description.mipCount;
            backendResource->resourceDescription.depth = inFfxResource->description.depth;
            break;

        case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
            reinterpret_cast<ID3D11Texture2D*>(dx11Resource)->GetDesc(&dx11Texture2DDesc);
            bindFlags = dx11Texture2DDesc.BindFlags;
            textureMipLevels = dx11Texture2DDesc.MipLevels;
            descFormat = patchDxgiFormatWithFfxUsage(dx11Texture2DDesc.Format, inFfxResource->description.format);
            dx11UavDescription.Format = convertFormatUav(descFormat);
            dx11SrvDescription.Format = convertFormatSrv(descFormat);
            if (dx11Texture2DDesc.ArraySize > 1 || requestArrayView)
            {
                dx11UavDescription.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
                dx11UavDescription.Texture2DArray.ArraySize = dx11Texture2DDesc.ArraySize;
                dx11UavDescription.Texture2DArray.FirstArraySlice = 0;
                dx11UavDescription.Texture2DArray.MipSlice = 0;

                dx11SrvDescription.ViewDimension = inFfxResource->description.type == FFX_RESOURCE_TYPE_TEXTURE_CUBE ? D3D11_SRV_DIMENSION_TEXTURECUBE : D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                dx11SrvDescription.Texture2DArray.ArraySize = dx11Texture2DDesc.ArraySize;
                dx11SrvDescription.Texture2DArray.FirstArraySlice = 0;
                dx11SrvDescription.Texture2DArray.MipLevels = dx11Texture2DDesc.MipLevels;
                dx11SrvDescription.Texture2DArray.MostDetailedMip = 0;
            }
            else
            {
                dx11UavDescription.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
                dx11UavDescription.Texture2D.MipSlice = 0;

                dx11SrvDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                dx11SrvDescription.Texture2D.MipLevels = dx11Texture2DDesc.MipLevels;
                dx11SrvDescription.Texture2D.MostDetailedMip = 0;
            }

            backendResource->resourceDescription.type = FFX_RESOURCE_TYPE_TEXTURE2D;
            backendResource->resourceDescription.format = inFfxResource->description.format;
            backendResource->resourceDescription.width = inFfxResource->description.width;
            backendResource->resourceDescription.height = inFfxResource->description.height;
            backendResource->resourceDescription.mipCount = inFfxResource->description.mipCount;
            backendResource->resourceDescription.depth = inFfxResource->description.depth;
            break;

        case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
            reinterpret_cast<ID3D11Texture3D*>(dx11Resource)->GetDesc(&dx11Texture3DDesc);
            bindFlags = dx11Texture3DDesc.BindFlags;
            textureMipLevels = dx11Texture3DDesc.MipLevels;
            descFormat = patchDxgiFormatWithFfxUsage(dx11Texture3DDesc.Format, inFfxResource->description.format);
            dx11UavDescription.Format = convertFormatUav(descFormat);
            dx11SrvDescription.Format = convertFormatSrv(descFormat);
            dx11UavDescription.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
            dx11SrvDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
            dx11SrvDescription.Texture3D.MipLevels = dx11Texture3DDesc.MipLevels;
            dx11SrvDescription.Texture3D.MostDetailedMip = 0;

            backendResource->resourceDescription.type = FFX_RESOURCE_TYPE_TEXTURE3D;
            backendResource->resourceDescription.format = inFfxResource->description.format;
            backendResource->resourceDescription.width = inFfxResource->description.width;
            backendResource->resourceDescription.height = inFfxResource->description.height;
            backendResource->resourceDescription.mipCount = inFfxResource->description.mipCount;
            backendResource->resourceDescription.depth = inFfxResource->description.depth;
            break;

        default:
            return FFX_ERROR_INVALID_ARGUMENT;
        }

        if (resourceDimension == D3D11_RESOURCE_DIMENSION_BUFFER) {

            const uint32_t stride = backendResource->resourceDescription.stride;
            if (stride == 0)
                return FFX_ERROR_INVALID_ARGUMENT;   // views stay null -> bound as NULL
            const UINT numElements = backendResource->resourceDescription.size / stride;

            // UAV
            if (bindFlags & D3D11_BIND_UNORDERED_ACCESS) {

                dx11UavDescription.Buffer.FirstElement = 0;
                dx11UavDescription.Buffer.NumElements = numElements;

                TIF(tf2fgInjectedFault() ? E_OUTOFMEMORY : dx11Device->CreateUnorderedAccessView(dx11Resource, &dx11UavDescription, &backendResource->uavPtr[0]));
            }
            if (bindFlags & D3D11_BIND_SHADER_RESOURCE)
            {
                dx11SrvDescription.Buffer.FirstElement        = 0;
                dx11SrvDescription.Buffer.NumElements         = numElements;

                TIF(tf2fgInjectedFault() ? E_OUTOFMEMORY : dx11Device->CreateShaderResourceView(dx11Resource, &dx11SrvDescription, &backendResource->srvPtr));
            }
        }
        else {

            // CPU readable
            if (bindFlags & D3D11_BIND_SHADER_RESOURCE)
                TIF(tf2fgInjectedFault() ? E_OUTOFMEMORY : dx11Device->CreateShaderResourceView(dx11Resource, &dx11SrvDescription, &backendResource->srvPtr));

            // UAV
            if (bindFlags & D3D11_BIND_UNORDERED_ACCESS) {

                const int32_t uavSlots = int32_t(sizeof(backendResource->uavPtr) / sizeof(backendResource->uavPtr[0]));
                int32_t uavDescriptorsCount = int32_t(backendResource->resourceDescription.mipCount);
                if (uavDescriptorsCount < 1)
                    uavDescriptorsCount = 1;
                if (uavDescriptorsCount > int32_t(textureMipLevels))
                    uavDescriptorsCount = int32_t(textureMipLevels);
                if (uavDescriptorsCount > uavSlots)
                    uavDescriptorsCount = uavSlots;
                backendResource->resourceDescription.mipCount = uint32_t(uavDescriptorsCount);

                for (int32_t currentMipIndex = 0; currentMipIndex < uavDescriptorsCount; ++currentMipIndex) {

                    // TextureXD<Y>.MipSlice values map to same mem
                    dx11UavDescription.Texture2D.MipSlice = currentMipIndex;

                    TIF(tf2fgInjectedFault() ? E_OUTOFMEMORY : dx11Device->CreateUnorderedAccessView(dx11Resource, &dx11UavDescription, &backendResource->uavPtr[currentMipIndex]));
                }
            }
        }
    }

    return FFX_OK;
}

FfxResource GetResourceDX11(FfxInterface* backendInterface, FfxResourceInternal inResource)
{
    FFX_ASSERT(nullptr != backendInterface);
    BackendContext_DX11* backendContext = (BackendContext_DX11*)backendInterface->scratchBuffer;

    FfxResource resource = {};
    // TF2FG PATCH (B-SAFE): out-of-range index -> empty resource
    if (!tf2fgValidResourceIndex(backendContext, inResource.internalIndex))
        return resource;

    FfxResourceDescription ffxResDescription = backendInterface->fpGetResourceDescription(backendInterface, inResource);

    resource.resource = resource.resource = reinterpret_cast<void*>(backendContext->pResources[inResource.internalIndex].resourcePtr);
    resource.state = FFX_RESOURCE_STATE_COMMON;
    resource.description = ffxResDescription;

#ifdef _DEBUG
    if (backendContext->pResources[inResource.internalIndex].resourceName)
    {
        wcscpy_s(resource.name, backendContext->pResources[inResource.internalIndex].resourceName);
    }
#endif

    return resource;
}


// dispose dynamic resources: This should be called at the end of the frame
FfxErrorCode UnregisterResourcesDX11(FfxInterface* backendInterface, FfxCommandList commandList, FfxUInt32 effectContextId)
{
    FFX_ASSERT(NULL != backendInterface);
    BackendContext_DX11* backendContext = (BackendContext_DX11*)(backendInterface->scratchBuffer);
    // TF2FG PATCH (B-SAFE): bounds check
    if (!tf2fgValidEffectContext(backendContext, effectContextId))
        return FFX_ERROR_INVALID_ARGUMENT;
    BackendContext_DX11::EffectContext& effectContext = backendContext->pEffectContexts[effectContextId];

    // Walk back all the resources that don't belong to us and reset them to their initial state
    for (uint32_t resourceIndex = ++effectContext.nextDynamicResource; resourceIndex < (effectContextId * FFX_MAX_RESOURCE_COUNT) + FFX_MAX_RESOURCE_COUNT; ++resourceIndex)
    {
        FfxResourceInternal internalResource;
        internalResource.internalIndex = resourceIndex;
        //DestroyResourceDX11(backendInterface, internalResource, effectContextId);
    }

    effectContext.nextDynamicResource      = (effectContextId * FFX_MAX_RESOURCE_COUNT) + FFX_MAX_RESOURCE_COUNT - 1;

    return FFX_OK;
}

FfxResourceDescription GetResourceDescriptorDX11(
    FfxInterface* backendInterface,
    FfxResourceInternal resource)
{
    FFX_ASSERT(NULL != backendInterface);

    BackendContext_DX11* backendContext = (BackendContext_DX11*)backendInterface->scratchBuffer;

    // TF2FG PATCH (B-SAFE): out-of-range index -> empty description
    if (!tf2fgValidResourceIndex(backendContext, resource.internalIndex))
        return FfxResourceDescription{};
    FfxResourceDescription resourceDescription = backendContext->pResources[resource.internalIndex].resourceDescription;
    return resourceDescription;
}

D3D11_TEXTURE_ADDRESS_MODE FfxGetAddressModeDX11(const FfxAddressMode& addressMode)
{
    switch (addressMode)
    {
    case FFX_ADDRESS_MODE_WRAP:
        return D3D11_TEXTURE_ADDRESS_WRAP;
    case FFX_ADDRESS_MODE_MIRROR:
        return D3D11_TEXTURE_ADDRESS_MIRROR;
    case FFX_ADDRESS_MODE_CLAMP:
        return D3D11_TEXTURE_ADDRESS_CLAMP;
    case FFX_ADDRESS_MODE_BORDER:
        return D3D11_TEXTURE_ADDRESS_BORDER;
    case FFX_ADDRESS_MODE_MIRROR_ONCE:
        return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    default:
        FFX_ASSERT_MESSAGE(false, "Unsupported addressing mode requested. Please implement");
        return D3D11_TEXTURE_ADDRESS_WRAP;
        break;
    }
}

static D3D11_FILTER FfxGetFilterTypeDX11(const FfxFilterType& filterType)
{
    switch (filterType)
    {
    case FFX_FILTER_TYPE_MINMAGMIP_POINT:
        return D3D11_FILTER_MIN_MAG_MIP_POINT;
    case FFX_FILTER_TYPE_MINMAGMIP_LINEAR:
        return D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    case FFX_FILTER_TYPE_MINMAGLINEARMIP_POINT:
        return D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    default:
        FFX_ASSERT_MESSAGE(false, "Unsupported sampler filter requested. Please implement");
        return D3D11_FILTER_MIN_MAG_MIP_POINT;
    }
}

// TF2FG PATCH (B-PIPE): rewritten with the original's structure, plus
//   - outPipeline->pipeline is nulled first, so any failure leaves a detectable null pipeline (the components now
//     propagate the error and refuse to dispatch; a null pipeline used to be an access violation),
//   - a missing/empty blob (permutation not built: accessor returns an empty blob) is an error, not an FFX_ASSERT,
//   - binding counts are bounds-checked against the FfxPipelineState arrays, constant buffers must be at b0..bN-1
//     (they are bound by array index), and every name copy is length-safe,
//   - the DXBC GroupMemoryBarrier -> GroupMemoryBarrierWithGroupSync patch (kept: the SM5 groupshared fallbacks
//     rely on it) is bounds-checked against the blob size.
FfxErrorCode CreatePipelineDX11(
    FfxInterface* backendInterface,
    FfxEffect effect,
    FfxPass pass,
    uint32_t permutationOptions,
    const FfxPipelineDescription* pipelineDescription,
    FfxUInt32                     effectContextId,
    FfxPipelineState* outPipeline)
{
    if (!backendInterface || !pipelineDescription || !outPipeline)
        return FFX_ERROR_INVALID_POINTER;

    BackendContext_DX11* backendContext = (BackendContext_DX11*)backendInterface->scratchBuffer;
    ID3D11Device* dx11Device = backendContext ? backendContext->device : nullptr;
    outPipeline->pipeline = nullptr;
    if (!dx11Device)
        return FFX_ERROR_NULL_DEVICE;

    if (pipelineDescription->stage == FfxBindStage(FFX_BIND_VERTEX_SHADER_STAGE | FFX_BIND_PIXEL_SHADER_STAGE))
    {
        return FFX_ERROR_BACKEND_API_ERROR;
    }
    else
    {
        FfxShaderBlob shaderBlob = { };
        const FfxErrorCode blobError = ffxGetPermutationBlobByIndex(effect, pass, FFX_BIND_COMPUTE_SHADER_STAGE, permutationOptions, &shaderBlob);
        if (blobError != FFX_OK || !shaderBlob.data || shaderBlob.size < 32 || (shaderBlob.size % sizeof(DWORD)) != 0)
            return FFX_ERROR_BACKEND_API_ERROR;

        if (shaderBlob.srvTextureCount > FFX_MAX_NUM_SRVS || shaderBlob.uavTextureCount > FFX_MAX_NUM_UAVS ||
            shaderBlob.srvBufferCount > FFX_MAX_NUM_SRVS || shaderBlob.uavBufferCount > FFX_MAX_NUM_UAVS ||
            shaderBlob.cbvCount > FFX_MAX_NUM_CONST_BUFFERS)
            return FFX_ERROR_OUT_OF_RANGE;

        // Only set the command signature if this is setup as an indirect workload
        outPipeline->cmdSignature = nullptr;

        // populate the pass.
        outPipeline->srvTextureCount = shaderBlob.srvTextureCount;
        outPipeline->uavTextureCount = shaderBlob.uavTextureCount;
        outPipeline->srvBufferCount = shaderBlob.srvBufferCount;
        outPipeline->uavBufferCount = shaderBlob.uavBufferCount;

        // Todo when needed
        //outPipeline->samplerCount      = shaderBlob.samplerCount;
        //outPipeline->rtAccelStructCount= shaderBlob.rtAccelStructCount;

        outPipeline->constCount = shaderBlob.cbvCount;
        for (uint32_t srvIndex = 0; srvIndex < outPipeline->srvTextureCount; ++srvIndex)
        {
            outPipeline->srvTextureBindings[srvIndex].slotIndex = shaderBlob.boundSRVTextures[srvIndex];
            outPipeline->srvTextureBindings[srvIndex].bindCount = shaderBlob.boundSRVTextureCounts[srvIndex];
            tf2fgCopyName(outPipeline->srvTextureBindings[srvIndex].name, shaderBlob.boundSRVTextureNames[srvIndex]);
            if (outPipeline->srvTextureBindings[srvIndex].bindCount != 1 ||
                outPipeline->srvTextureBindings[srvIndex].slotIndex >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
                return FFX_ERROR_OUT_OF_RANGE;
        }
        for (uint32_t uavIndex = 0; uavIndex < outPipeline->uavTextureCount; ++uavIndex)
        {
            outPipeline->uavTextureBindings[uavIndex].slotIndex = shaderBlob.boundUAVTextures[uavIndex];
            outPipeline->uavTextureBindings[uavIndex].bindCount = shaderBlob.boundUAVTextureCounts[uavIndex];
            tf2fgCopyName(outPipeline->uavTextureBindings[uavIndex].name, shaderBlob.boundUAVTextureNames[uavIndex]);
            if (outPipeline->uavTextureBindings[uavIndex].bindCount != 1 ||
                outPipeline->uavTextureBindings[uavIndex].slotIndex >= D3D11_1_UAV_SLOT_COUNT)
                return FFX_ERROR_OUT_OF_RANGE;
        }
        for (uint32_t srvIndex = 0; srvIndex < outPipeline->srvBufferCount; ++srvIndex)
        {
            outPipeline->srvBufferBindings[srvIndex].slotIndex = shaderBlob.boundSRVBuffers[srvIndex];
            outPipeline->srvBufferBindings[srvIndex].bindCount = shaderBlob.boundSRVBufferCounts[srvIndex];
            tf2fgCopyName(outPipeline->srvBufferBindings[srvIndex].name, shaderBlob.boundSRVBufferNames[srvIndex]);
            if (outPipeline->srvBufferBindings[srvIndex].bindCount != 1 ||
                outPipeline->srvBufferBindings[srvIndex].slotIndex >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
                return FFX_ERROR_OUT_OF_RANGE;
        }
        for (uint32_t uavIndex = 0; uavIndex < outPipeline->uavBufferCount; ++uavIndex)
        {
            outPipeline->uavBufferBindings[uavIndex].slotIndex = shaderBlob.boundUAVBuffers[uavIndex];
            outPipeline->uavBufferBindings[uavIndex].bindCount = shaderBlob.boundUAVBufferCounts[uavIndex];
            tf2fgCopyName(outPipeline->uavBufferBindings[uavIndex].name, shaderBlob.boundUAVBufferNames[uavIndex]);
            if (outPipeline->uavBufferBindings[uavIndex].bindCount != 1 ||
                outPipeline->uavBufferBindings[uavIndex].slotIndex >= D3D11_1_UAV_SLOT_COUNT)
                return FFX_ERROR_OUT_OF_RANGE;
        }
        for (uint32_t cbIndex = 0; cbIndex < outPipeline->constCount; ++cbIndex)
        {
            outPipeline->constantBufferBindings[cbIndex].slotIndex = shaderBlob.boundConstantBuffers[cbIndex];
            outPipeline->constantBufferBindings[cbIndex].bindCount = shaderBlob.boundConstantBufferCounts[cbIndex];
            tf2fgCopyName(outPipeline->constantBufferBindings[cbIndex].name, shaderBlob.boundConstantBufferNames[cbIndex]);
            // executeGpuJobCompute binds constant buffer i at slot i
            if (outPipeline->constantBufferBindings[cbIndex].slotIndex != cbIndex)
                return FFX_ERROR_OUT_OF_RANGE;
        }

        // patch GroupMemoryBarrier to GroupMemoryBarrierWithGroupSync
        const uint32_t dwordCount = shaderBlob.size / sizeof(DWORD);
        DWORD* data = new (std::nothrow) DWORD[dwordCount];
        if (data == nullptr)
            return FFX_ERROR_INSUFFICIENT_MEMORY;
        memcpy(data, shaderBlob.data, shaderBlob.size);
        for (uint32_t i = 0; i + 1 < dwordCount; ++i)
        {
            if (data[i] == MAKEFOURCC('S','H','E','X'))
            {
                DWORD* shex = &data[i];
                const DWORD count = data[i + 1];
                const uint32_t available = dwordCount - i;       // dwords from the chunk header to the end of the blob
                uint32_t limit = count / sizeof(DWORD);
                if (limit > available)
                    limit = available;
                bool hash = false;
                for (uint32_t j = 0; j < limit; ++j)
                {
                    if (shex[j] == 0x010010BE)
                    {
                        shex[j] = 0x010018BE;
                        hash = true;
                    }
                }
                if (hash)
                {
                    CalculateDXBCChecksum(data, shaderBlob.size, &data[1]);
                }
                break;
            }
        }

        PipelineState_DX11* dx11Pipeline = new (std::nothrow) PipelineState_DX11();
        if (dx11Pipeline == nullptr)
        {
            delete[] data;
            return FFX_ERROR_INSUFFICIENT_MEMORY;
        }

        // create the compute shader
        if (FAILED(dx11Device->CreateComputeShader(data, shaderBlob.size, nullptr, &dx11Pipeline->computeShader)) || !dx11Pipeline->computeShader)
        {
            delete dx11Pipeline;
            delete[] data;
            return FFX_ERROR_BACKEND_API_ERROR;
        }
        delete[] data;

        // DX12 gets these as static samplers from the root signature. DX11 must
        // create and bind the sampler states explicitly.
        if (pipelineDescription->samplerCount > FFX_MAX_SAMPLERS ||
            (pipelineDescription->samplerCount > 0 && pipelineDescription->samplers == nullptr))
        {
            dx11Pipeline->computeShader->Release();
            delete dx11Pipeline;
            return FFX_ERROR_BACKEND_API_ERROR;
        }

        dx11Pipeline->samplerCount = static_cast<uint32_t>(pipelineDescription->samplerCount);
        for (uint32_t samplerIndex = 0; samplerIndex < dx11Pipeline->samplerCount; ++samplerIndex)
        {
            const FfxSamplerDescription& ffxSampler = pipelineDescription->samplers[samplerIndex];

            D3D11_SAMPLER_DESC samplerDesc = {};
            samplerDesc.Filter = FfxGetFilterTypeDX11(ffxSampler.filter);
            samplerDesc.AddressU = FfxGetAddressModeDX11(ffxSampler.addressModeU);
            samplerDesc.AddressV = FfxGetAddressModeDX11(ffxSampler.addressModeV);
            samplerDesc.AddressW = FfxGetAddressModeDX11(ffxSampler.addressModeW);
            samplerDesc.MipLODBias = 0.0f;
            samplerDesc.MaxAnisotropy = 1;
            samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
            samplerDesc.MinLOD = 0.0f;
            samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

            if (FAILED(dx11Device->CreateSamplerState(&samplerDesc, &dx11Pipeline->samplerStates[samplerIndex])) || !dx11Pipeline->samplerStates[samplerIndex])
            {
                for (uint32_t releaseIndex = 0; releaseIndex < samplerIndex; ++releaseIndex)
                    dx11Pipeline->samplerStates[releaseIndex]->Release();

                dx11Pipeline->computeShader->Release();
                delete dx11Pipeline;
                return FFX_ERROR_BACKEND_API_ERROR;
            }
        }

        outPipeline->pipeline = reinterpret_cast<FfxPipeline>(dx11Pipeline);

        // Set the pipeline name.
        SetNameDX11(dx11Pipeline->computeShader, pipelineDescription->name);
    }
    return FFX_OK;
}

FfxErrorCode DestroyPipelineDX11(
    FfxInterface* backendInterface,
    FfxPipelineState* pipeline,
    FfxUInt32 effectContextId)
{
    FFX_ASSERT(backendInterface != nullptr);
    if (!pipeline) {
        return FFX_OK;
    }

    // destroy pipeline
    PipelineState_DX11* dx11Pipeline = reinterpret_cast<PipelineState_DX11*>(pipeline->pipeline);
    if (dx11Pipeline) {
        for (uint32_t samplerIndex = 0; samplerIndex < dx11Pipeline->samplerCount; ++samplerIndex) {
            if (dx11Pipeline->samplerStates[samplerIndex])
                dx11Pipeline->samplerStates[samplerIndex]->Release();
        }

        if (dx11Pipeline->computeShader)
            dx11Pipeline->computeShader->Release();

        delete dx11Pipeline;
    }
    pipeline->pipeline = nullptr;

    return FFX_OK;
}

// TF2FG PATCH (B-JOBS): pGpuJobs holds maxEffectContexts * FFX_MAX_GPU_JOBS entries and gpuJobCount is shared by all
// contexts. The original only FFX_ASSERTed (compiled out) against FFX_MAX_GPU_JOBS; an OF reset dispatch alone
// schedules 68 jobs. Bound-check against the real array size, drop + flag on overflow (the next Execute refuses
// the whole batch), and clamp constant-buffer copies to FfxConstantBuffer::data.
FfxErrorCode ScheduleGpuJobDX11(
    FfxInterface* backendInterface,
    const FfxGpuJobDescription* job
)
{
    if (!backendInterface || !job)
        return FFX_ERROR_INVALID_POINTER;

    BackendContext_DX11* backendContext = (BackendContext_DX11*)backendInterface->scratchBuffer;
    if (!backendContext || !backendContext->pGpuJobs)
        return FFX_ERROR_INVALID_POINTER;

    const uint32_t capacity = backendContext->maxEffectContexts * FFX_MAX_GPU_JOBS;
    if (backendContext->gpuJobCount >= capacity) {
        backendContext->gpuJobOverflow = true;
        return FFX_ERROR_OUT_OF_RANGE;
    }

    backendContext->pGpuJobs[backendContext->gpuJobCount] = *job;

    if (job->jobType == FFX_GPU_JOB_COMPUTE) {

        // needs to copy SRVs and UAVs in case they are on the stack only
        FfxComputeJobDescription* computeJob = &backendContext->pGpuJobs[backendContext->gpuJobCount].computeJobDescriptor;
        uint32_t numConstBuffers = job->computeJobDescriptor.pipeline.constCount;
        if (numConstBuffers > FFX_MAX_NUM_CONST_BUFFERS)
            numConstBuffers = FFX_MAX_NUM_CONST_BUFFERS;
        for (uint32_t currentRootConstantIndex = 0; currentRootConstantIndex< numConstBuffers; ++currentRootConstantIndex)
        {
            uint32_t entries = job->computeJobDescriptor.cbs[currentRootConstantIndex].num32BitEntries;
            if (entries > FFX_MAX_CONST_SIZE)
                entries = FFX_MAX_CONST_SIZE;
            computeJob->cbs[currentRootConstantIndex].num32BitEntries = entries;
            memcpy(computeJob->cbs[currentRootConstantIndex].data, job->computeJobDescriptor.cbs[currentRootConstantIndex].data, entries*sizeof(uint32_t));
        }
    }

    backendContext->gpuJobCount++;

    return FFX_OK;
}

// TF2FG PATCH (B-DISPATCH): rewritten with the original's structure, plus
//   - the pipeline / compute shader is validated BEFORE anything is bound (a null pipeline used to be an access
//     violation at dx11Pipeline->computeShader = game death under Northstar),
//   - every resource index, mip index, slot index and count is bounds-checked,
//   - an unregistered SRV (internalIndex 0, e.g. OPTICAL_FLOW_CONFIDENCE which FI never registers) is bound as NULL.
//     The original 'break'-ed and left whatever SRV the caller had at that slot (stale game SRV read on DX11).
static ID3D11UnorderedAccessView* tf2fgUav(BackendContext_DX11* backendContext, int32_t resourceIndex, uint32_t mip)
{
    if (resourceIndex <= 0 || !tf2fgValidResourceIndex(backendContext, resourceIndex) || mip >= 16)
        return nullptr;
    return backendContext->pResources[resourceIndex].uavPtr[mip];
}

static ID3D11ShaderResourceView* tf2fgSrv(BackendContext_DX11* backendContext, int32_t resourceIndex)
{
    if (resourceIndex <= 0 || !tf2fgValidResourceIndex(backendContext, resourceIndex))
        return nullptr;
    return backendContext->pResources[resourceIndex].srvPtr;
}

static FfxErrorCode executeGpuJobCompute(BackendContext_DX11* backendContext, FfxGpuJobDescription* job, ID3D11Device* dx11Device, ID3D11DeviceContext* dx11DeviceContext)
{
    const FfxPipelineState& pipeline = job->computeJobDescriptor.pipeline;

    // validate first: nothing is bound if the job cannot run
    PipelineState_DX11* dx11Pipeline = reinterpret_cast<PipelineState_DX11*>(pipeline.pipeline);
    if (dx11Pipeline == nullptr || dx11Pipeline->computeShader == nullptr)
        return FFX_ERROR_BACKEND_API_ERROR;
    if (pipeline.uavTextureCount > FFX_MAX_NUM_UAVS || pipeline.uavBufferCount > FFX_MAX_NUM_UAVS ||
        pipeline.srvTextureCount > FFX_MAX_NUM_SRVS || pipeline.srvBufferCount > FFX_MAX_NUM_SRVS ||
        pipeline.constCount > FFX_MAX_NUM_CONST_BUFFERS || dx11Pipeline->samplerCount > FFX_MAX_SAMPLERS)
        return FFX_ERROR_OUT_OF_RANGE;
    for (uint32_t i = 0; i < pipeline.uavTextureCount; ++i)
        if (pipeline.uavTextureBindings[i].bindCount != 1 || pipeline.uavTextureBindings[i].slotIndex >= D3D11_1_UAV_SLOT_COUNT)
            return FFX_ERROR_OUT_OF_RANGE;
    for (uint32_t i = 0; i < pipeline.uavBufferCount; ++i)
        if (pipeline.uavBufferBindings[i].slotIndex >= D3D11_1_UAV_SLOT_COUNT)
            return FFX_ERROR_OUT_OF_RANGE;
    for (uint32_t i = 0; i < pipeline.srvTextureCount; ++i)
        if (pipeline.srvTextureBindings[i].bindCount != 1 || pipeline.srvTextureBindings[i].slotIndex >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
            return FFX_ERROR_OUT_OF_RANGE;
    for (uint32_t i = 0; i < pipeline.srvBufferCount; ++i)
        if (pipeline.srvBufferBindings[i].slotIndex >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
            return FFX_ERROR_OUT_OF_RANGE;
    for (uint32_t i = 0; i < pipeline.constCount; ++i)
        if (job->computeJobDescriptor.cbs[i].num32BitEntries > FFX_MAX_CONST_SIZE)
            return FFX_ERROR_OUT_OF_RANGE;

    // bind texture & buffer UAVs
    uint32_t minimumUav = UINT32_MAX;
    uint32_t maximumUav = 0;
    {
        // Textures (bindCount validated == 1, so binding index == job array index)
        for (uint32_t i = 0; i < pipeline.uavTextureCount; ++i) {
            ID3D11UnorderedAccessView* uavPtr = tf2fgUav(backendContext, job->computeJobDescriptor.uavTextures[i].internalIndex,
                                                         job->computeJobDescriptor.uavTextureMips[i]);
            const uint32_t slot = pipeline.uavTextureBindings[i].slotIndex;
#if defined(TF2FG_TESTING)
            if (!uavPtr) tf2fgReportNullBinding(dx11Pipeline->computeShader, pipeline.uavTextureBindings[i].name, 1);
#endif
            dx11DeviceContext->CSSetUnorderedAccessViews(slot, 1, &uavPtr, nullptr);
            minimumUav = minimumUav < slot ? minimumUav : slot;
            maximumUav = maximumUav > slot ? maximumUav : slot;
        }

        // Buffers
        for (uint32_t i = 0; i < pipeline.uavBufferCount; ++i) {
            ID3D11UnorderedAccessView* uavPtr = tf2fgUav(backendContext, job->computeJobDescriptor.uavBuffers[i].internalIndex, 0);
            const uint32_t slot = pipeline.uavBufferBindings[i].slotIndex;
#if defined(TF2FG_TESTING)
            if (!uavPtr) tf2fgReportNullBinding(dx11Pipeline->computeShader, pipeline.uavBufferBindings[i].name, 1);
#endif
            dx11DeviceContext->CSSetUnorderedAccessViews(slot, 1, &uavPtr, nullptr);
            minimumUav = minimumUav < slot ? minimumUav : slot;
            maximumUav = maximumUav > slot ? maximumUav : slot;
        }
    }

    // bind texture & buffer SRVs
    uint32_t minimumSrv = UINT32_MAX;
    uint32_t maximumSrv = 0;
    {
        for (uint32_t i = 0; i < pipeline.srvTextureCount; ++i)
        {
            ID3D11ShaderResourceView* srvPtr = tf2fgSrv(backendContext, job->computeJobDescriptor.srvTextures[i].internalIndex); // NULL when unregistered
            const uint32_t slot = pipeline.srvTextureBindings[i].slotIndex;
#if defined(TF2FG_TESTING)
            if (!srvPtr) tf2fgReportNullBinding(dx11Pipeline->computeShader, pipeline.srvTextureBindings[i].name, 0);
#endif
            dx11DeviceContext->CSSetShaderResources(slot, 1, &srvPtr);
            minimumSrv = minimumSrv < slot ? minimumSrv : slot;
            maximumSrv = maximumSrv > slot ? maximumSrv : slot;
        }

        for (uint32_t i = 0; i < pipeline.srvBufferCount; ++i)
        {
            ID3D11ShaderResourceView* srvPtr = tf2fgSrv(backendContext, job->computeJobDescriptor.srvBuffers[i].internalIndex);
            const uint32_t slot = pipeline.srvBufferBindings[i].slotIndex;
#if defined(TF2FG_TESTING)
            if (!srvPtr) tf2fgReportNullBinding(dx11Pipeline->computeShader, pipeline.srvBufferBindings[i].name, 0);
#endif
            dx11DeviceContext->CSSetShaderResources(slot, 1, &srvPtr);
            minimumSrv = minimumSrv < slot ? minimumSrv : slot;
            maximumSrv = maximumSrv > slot ? maximumSrv : slot;
        }
    }

    // bind pipeline
    dx11DeviceContext->CSSetShader(dx11Pipeline->computeShader, nullptr, 0);

    if (dx11Pipeline->samplerCount > 0) {
        dx11DeviceContext->CSSetSamplers(0, dx11Pipeline->samplerCount, dx11Pipeline->samplerStates);
    }

    // copy data to constant buffer and bind
    {
        std::lock_guard<std::mutex> cbLock{ backendContext->constantBufferMutex };
        for (uint32_t currentRootConstantIndex = 0; currentRootConstantIndex < pipeline.constCount; ++currentRootConstantIndex) {

            uint32_t size = FFX_ALIGN_UP(job->computeJobDescriptor.cbs[currentRootConstantIndex].num32BitEntries * sizeof(uint32_t), 256);
            if (size == 0)
                size = 256;

            if (backendContext->constantBufferSize[currentRootConstantIndex] < size) {

                if (backendContext->constantBufferResource[currentRootConstantIndex] != NULL) {
                    backendContext->constantBufferResource[currentRootConstantIndex]->Release();
                    backendContext->constantBufferResource[currentRootConstantIndex] = NULL;
                }
                backendContext->constantBufferSize[currentRootConstantIndex] = 0;

                D3D11_BUFFER_DESC constDesc = {};
                constDesc.ByteWidth = size;
                constDesc.Usage = D3D11_USAGE_DYNAMIC;
                constDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                constDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                TIF(tf2fgInjectedFault() ? E_OUTOFMEMORY : dx11Device->CreateBuffer(&constDesc, nullptr, &backendContext->constantBufferResource[currentRootConstantIndex]));
                backendContext->constantBufferSize[currentRootConstantIndex] = size;
                SetNameDX11(backendContext->constantBufferResource[currentRootConstantIndex], L"FFX_DX11_ConstantBuffer");
            }

            if (backendContext->constantBufferResource[currentRootConstantIndex] != NULL) {

                D3D11_MAPPED_SUBRESOURCE mappedSubresource = {};
                TIF(tf2fgInjectedFaultMap() ? E_OUTOFMEMORY : backendContext->deviceContext->Map(backendContext->constantBufferResource[currentRootConstantIndex], 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedSubresource));
                if (!mappedSubresource.pData)
                    return FFX_ERROR_BACKEND_API_ERROR;

                memcpy(mappedSubresource.pData, job->computeJobDescriptor.cbs[currentRootConstantIndex].data,
                       job->computeJobDescriptor.cbs[currentRootConstantIndex].num32BitEntries * sizeof(uint32_t));
                backendContext->deviceContext->Unmap(backendContext->constantBufferResource[currentRootConstantIndex], 0);

                dx11DeviceContext->CSSetConstantBuffers(currentRootConstantIndex, 1, &backendContext->constantBufferResource[currentRootConstantIndex]);
            }
        }
    }

    // dispatch
    dx11DeviceContext->Dispatch(job->computeJobDescriptor.dimensions[0], job->computeJobDescriptor.dimensions[1], job->computeJobDescriptor.dimensions[2]);

    // unbind UAVs
    static ID3D11UnorderedAccessView* const emptyUAVs[D3D11_1_UAV_SLOT_COUNT] = {};
    if (minimumUav <= maximumUav) {

        uint32_t count = maximumUav - minimumUav + 1;
        dx11DeviceContext->CSSetUnorderedAccessViews(minimumUav, count, emptyUAVs, nullptr);
    }

    // unbind SRVs
    static ID3D11ShaderResourceView* const emptySRVs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    if (minimumSrv <= maximumSrv) {

        uint32_t count = maximumSrv - minimumSrv + 1;
        dx11DeviceContext->CSSetShaderResources(minimumSrv, count, emptySRVs);
    }

    return FFX_OK;
}

static FfxErrorCode executeGpuJobCopy(BackendContext_DX11* backendContext, FfxGpuJobDescription* job, ID3D11Device* dx11Device, ID3D11DeviceContext* dx11DeviceContext)
{
    // TF2FG PATCH (B-SAFE): validate both ends
    const int32_t srcIndex = job->copyJobDescriptor.src.internalIndex;
    const int32_t dstIndex = job->copyJobDescriptor.dst.internalIndex;
    if (srcIndex <= 0 || dstIndex <= 0 || !tf2fgValidResourceIndex(backendContext, srcIndex) || !tf2fgValidResourceIndex(backendContext, dstIndex))
        return FFX_ERROR_INVALID_ARGUMENT;
    ID3D11Resource* dx11ResourceSrc = getDX11ResourcePtr_Fsr31(backendContext, srcIndex);
    ID3D11Resource* dx11ResourceDst = getDX11ResourcePtr_Fsr31(backendContext, dstIndex);
    if (!dx11ResourceSrc || !dx11ResourceDst)
        return FFX_ERROR_INVALID_ARGUMENT;

    dx11DeviceContext->CopyResource(dx11ResourceDst, dx11ResourceSrc);

    return FFX_OK;
}

static FfxErrorCode executeGpuJobClearFloat(BackendContext_DX11* backendContext, FfxGpuJobDescription* job, ID3D11Device* dx11Device, ID3D11DeviceContext* dx11DeviceContext)
{
    // TF2FG PATCH (B-SAFE): validate the index and the view (ClearUnorderedAccessViewUint on NULL is not allowed)
    const int32_t idx = job->clearJobDescriptor.target.internalIndex;
    ID3D11UnorderedAccessView* uav = tf2fgUav(backendContext, idx, 0);
    if (!uav)
        return FFX_ERROR_INVALID_ARGUMENT;

    uint32_t clearColorAsUint[4];
    memcpy(clearColorAsUint, job->clearJobDescriptor.color, sizeof(clearColorAsUint));
    dx11DeviceContext->ClearUnorderedAccessViewUint(uav, clearColorAsUint);

    return FFX_OK;
}

// TF2FG PATCH (B-EXEC): the job list is ALWAYS consumed (the original kept gpuJobCount on error, so the next call
// replayed stale jobs and overran the array), an overflowed batch is refused as a whole, the first failing job
// stops the batch, and a C++ exception from TIF inside a job is converted into an error code after the CS samplers
// are restored.
FfxErrorCode ExecuteGpuJobsDX11(
    FfxInterface* backendInterface,
    FfxCommandList commandList)
{
    if (!backendInterface)
        return FFX_ERROR_INVALID_POINTER;

    BackendContext_DX11* backendContext = (BackendContext_DX11*)backendInterface->scratchBuffer;
    if (!backendContext || !backendContext->deviceContext || !backendContext->device)
        return FFX_ERROR_NULL_DEVICE;

    FfxErrorCode errorCode = FFX_OK;

    if (backendContext->gpuJobOverflow) {
        backendContext->gpuJobCount = 0;
        backendContext->gpuJobOverflow = false;
        return FFX_ERROR_OUT_OF_RANGE;
    }

    // FFX binds its own compute samplers. Preserve the game's sampler state and
    // restore it after the complete FidelityFX job batch.
    ID3D11SamplerState* restoreSamplers[FFX_MAX_SAMPLERS] = {};
    backendContext->deviceContext->CSGetSamplers(0, FFX_MAX_SAMPLERS, restoreSamplers);

    try {
        // execute all GpuJobs
        for (uint32_t currentGpuJobIndex = 0; currentGpuJobIndex < backendContext->gpuJobCount && errorCode == FFX_OK; ++currentGpuJobIndex) {

            FfxGpuJobDescription* GpuJob = &backendContext->pGpuJobs[currentGpuJobIndex];
            ID3D11Device* dx11Device = backendContext->device;
            ID3D11DeviceContext* dx11DeviceContext = backendContext->deviceContext;

            switch (GpuJob->jobType) {

                case FFX_GPU_JOB_CLEAR_FLOAT:
                    errorCode = executeGpuJobClearFloat(backendContext, GpuJob, dx11Device, dx11DeviceContext);
                    break;

                case FFX_GPU_JOB_COPY:
                    errorCode = executeGpuJobCopy(backendContext, GpuJob, dx11Device, dx11DeviceContext);
                    break;

                case FFX_GPU_JOB_COMPUTE:
                    errorCode = executeGpuJobCompute(backendContext, GpuJob, dx11Device, dx11DeviceContext);
                    break;

                default:
                    break;
            }
        }
    }
    catch (...) {
        errorCode = FFX_ERROR_BACKEND_API_ERROR;
    }

    backendContext->deviceContext->CSSetSamplers(0, FFX_MAX_SAMPLERS, restoreSamplers);
    for (uint32_t samplerIndex = 0; samplerIndex < FFX_MAX_SAMPLERS; ++samplerIndex) {
        if (restoreSamplers[samplerIndex])
            restoreSamplers[samplerIndex]->Release();
    }

    backendContext->gpuJobCount = 0;

    // check the execute function returned cleanly.
    FFX_RETURN_ON_ERROR(
        errorCode == FFX_OK,
        FFX_ERROR_BACKEND_API_ERROR);

    return FFX_OK;
}
