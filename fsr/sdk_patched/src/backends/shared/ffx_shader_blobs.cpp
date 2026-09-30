// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
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

// TF2FG PATCH (A3): the tf2-fsr-fg build links only the frame interpolation and optical flow effects. The
// original dispatcher switched over every SDK effect behind FFX_FSR/FFX_SPD/... defines (FI/OF only under
// FFX_FSR3, which the fork's DX11 build never defines). Unknown effects now return an error and an empty blob
// instead of FFX_ASSERT_MESSAGE + FFX_OK.

#include "ffx_shader_blobs.h"
// TF2FG PATCH (A-FSR3): the tf2-dlss-sr library (lib/build_up.bat) links no FI/OF blobs: TF2FG_NO_FI_OF leaves them out.
#if !defined(TF2FG_NO_FI_OF)
#include "blob_accessors/ffx_frameinterpolation_shaderblobs.h"
#include "blob_accessors/ffx_opticalflow_shaderblobs.h"
#endif
// TF2FG PATCH (A-FSR3, 2026-09-29): the FSR 3.1 upscaler accessor is compiled in only for the tf2-dlss-sr library
// (lib/build_up.bat defines TF2FG_WITH_FSR3UPSCALER); the FG library (FI + OF) is unchanged without it.
#if defined(TF2FG_WITH_FSR3UPSCALER)
#include "blob_accessors/ffx_fsr3upscaler_shaderblobs.h"
#endif

#include <string.h> // for memset

FfxErrorCode ffxGetPermutationBlobByIndex(
    FfxEffect effectId,
    FfxPass passId,
    FfxBindStage stageId,
    uint32_t permutationOptions,
    FfxShaderBlob* outBlob)
{
    if (!outBlob)
        return FFX_ERROR_INVALID_POINTER;
    (void)stageId;

    switch (effectId)
    {
#if !defined(TF2FG_NO_FI_OF)
    case FFX_EFFECT_FRAMEINTERPOLATION:
        return frameInterpolationGetPermutationBlobByIndex((FfxFrameInterpolationPass)passId, stageId, permutationOptions, outBlob);
    case FFX_EFFECT_OPTICALFLOW:
        return opticalflowGetPermutationBlobByIndex((FfxOpticalflowPass)passId, permutationOptions, outBlob);
#endif
#if defined(TF2FG_WITH_FSR3UPSCALER)
    case FFX_EFFECT_FSR3UPSCALER:
        return fsr3UpscalerGetPermutationBlobByIndex((FfxFsr3UpscalerPass)passId, permutationOptions, outBlob);
#endif
    default:
        break;
    }

    // return an empty blob
    memset(outBlob, 0, sizeof(FfxShaderBlob));
    return FFX_ERROR_INVALID_ARGUMENT;
}

FfxErrorCode ffxIsWave64(FfxEffect effectId, uint32_t permutationOptions, bool& isWave64)
{
    switch (effectId)
    {
#if !defined(TF2FG_NO_FI_OF)
    case FFX_EFFECT_FRAMEINTERPOLATION:
        return frameInterpolationIsWave64(permutationOptions, isWave64);
    case FFX_EFFECT_OPTICALFLOW:
        return opticalflowIsWave64(permutationOptions, isWave64);
#endif
#if defined(TF2FG_WITH_FSR3UPSCALER)
    case FFX_EFFECT_FSR3UPSCALER:
        return fsr3UpscalerIsWave64(permutationOptions, isWave64);
#endif
    default:
        isWave64 = false;
        return FFX_ERROR_INVALID_ARGUMENT;
    }
}
