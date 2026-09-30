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

// TF2FG PATCH (A-FSR3, 2026-09-29): rewritten like the FI/OF accessors of this tree. Only the FFX_HALF=0, wave32 tables
// exist (shaders/build_shaders.py fsr3up -> shaders/generated_fsr3up; the DX11 backend reports no FP16 and no wave lanes),
// every table lookup is bounds-checked, and a permutation that was not compiled returns FFX_ERROR_INVALID_ARGUMENT with an
// empty blob (pipeline creation then fails cleanly) instead of silently using entry 0.
#include <FidelityFX/host/ffx_util.h>
#include "ffx_fsr3upscaler_shaderblobs.h"
#include "fsr3upscaler/ffx_fsr3upscaler_private.h"

#include <ffx_fsr3upscaler_autogen_reactive_pass_permutations.h>
#include <ffx_fsr3upscaler_accumulate_pass_permutations.h>
#include <ffx_fsr3upscaler_luma_pyramid_pass_permutations.h>
#include <ffx_fsr3upscaler_prepare_reactivity_pass_permutations.h>
#include <ffx_fsr3upscaler_prepare_inputs_pass_permutations.h>
#include <ffx_fsr3upscaler_shading_change_pass_permutations.h>
#include <ffx_fsr3upscaler_rcas_pass_permutations.h>
#include <ffx_fsr3upscaler_shading_change_pyramid_pass_permutations.h>
#include <ffx_fsr3upscaler_luma_instability_pass_permutations.h>
#include <ffx_fsr3upscaler_debug_view_pass_permutations.h>

#include <string.h> // for memset

#if defined(POPULATE_PERMUTATION_KEY)
#undef POPULATE_PERMUTATION_KEY
#endif // #if defined(POPULATE_PERMUTATION_KEY)
#define POPULATE_PERMUTATION_KEY(options, key)                                                                \
key.index = 0;                                                                                                \
key.FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE = FFX_CONTAINS_FLAG(options, FSR3UPSCALER_SHADER_PERMUTATION_USE_LANCZOS_TYPE);                     \
key.FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT = FFX_CONTAINS_FLAG(options, FSR3UPSCALER_SHADER_PERMUTATION_HDR_COLOR_INPUT);                 \
key.FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS = FFX_CONTAINS_FLAG(options, FSR3UPSCALER_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS);   \
key.FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS = FFX_CONTAINS_FLAG(options, FSR3UPSCALER_SHADER_PERMUTATION_JITTER_MOTION_VECTORS);   \
key.FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH = FFX_CONTAINS_FLAG(options, FSR3UPSCALER_SHADER_PERMUTATION_DEPTH_INVERTED);                   \
key.FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING = FFX_CONTAINS_FLAG(options, FSR3UPSCALER_SHADER_PERMUTATION_ENABLE_SHARPENING);

#define TF2FG_DEFINE_BLOB_GETTER(getterName, pass)                                                                 \
    static bool getterName(uint32_t permutationOptions, FfxShaderBlob* outBlob)                                    \
    {                                                                                                              \
        pass##_PermutationKey key;                                                                                 \
        POPULATE_PERMUTATION_KEY(permutationOptions, key);                                                         \
        const uint32_t tableSize = uint32_t(sizeof(g_##pass##_IndirectionTable) / sizeof(g_##pass##_IndirectionTable[0])); \
        if (key.index >= tableSize)                                                                                \
            return false;                                                                                          \
        const int32_t tableIndex = g_##pass##_IndirectionTable[key.index];                                         \
        if (tableIndex < 0 || uint32_t(tableIndex) >= g_##pass##_PermutationCount)                                 \
            return false;                                                                                          \
        FfxShaderBlob blob = POPULATE_SHADER_BLOB_FFX(g_##pass##_PermutationInfo, tableIndex);                    \
        memcpy(outBlob, &blob, sizeof(FfxShaderBlob));                                                             \
        return true;                                                                                               \
    }

TF2FG_DEFINE_BLOB_GETTER(getPrepareInputs, ffx_fsr3upscaler_prepare_inputs_pass)
TF2FG_DEFINE_BLOB_GETTER(getLumaPyramid, ffx_fsr3upscaler_luma_pyramid_pass)
TF2FG_DEFINE_BLOB_GETTER(getShadingChangePyramid, ffx_fsr3upscaler_shading_change_pyramid_pass)
TF2FG_DEFINE_BLOB_GETTER(getShadingChange, ffx_fsr3upscaler_shading_change_pass)
TF2FG_DEFINE_BLOB_GETTER(getPrepareReactivity, ffx_fsr3upscaler_prepare_reactivity_pass)
TF2FG_DEFINE_BLOB_GETTER(getLumaInstability, ffx_fsr3upscaler_luma_instability_pass)
TF2FG_DEFINE_BLOB_GETTER(getAccumulate, ffx_fsr3upscaler_accumulate_pass)
TF2FG_DEFINE_BLOB_GETTER(getRcas, ffx_fsr3upscaler_rcas_pass)
TF2FG_DEFINE_BLOB_GETTER(getAutogenReactive, ffx_fsr3upscaler_autogen_reactive_pass)
TF2FG_DEFINE_BLOB_GETTER(getDebugView, ffx_fsr3upscaler_debug_view_pass)

FfxErrorCode fsr3UpscalerGetPermutationBlobByIndex(
    FfxFsr3UpscalerPass passId,
    uint32_t permutationOptions,
    FfxShaderBlob* outBlob) {

    if (!outBlob)
        return FFX_ERROR_INVALID_POINTER;

    bool found = false;
    switch (passId) {
        case FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS:          found = getPrepareInputs(permutationOptions, outBlob); break;
        case FFX_FSR3UPSCALER_PASS_LUMA_PYRAMID:            found = getLumaPyramid(permutationOptions, outBlob); break;
        case FFX_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID:  found = getShadingChangePyramid(permutationOptions, outBlob); break;
        case FFX_FSR3UPSCALER_PASS_SHADING_CHANGE:          found = getShadingChange(permutationOptions, outBlob); break;
        case FFX_FSR3UPSCALER_PASS_PREPARE_REACTIVITY:      found = getPrepareReactivity(permutationOptions, outBlob); break;
        case FFX_FSR3UPSCALER_PASS_LUMA_INSTABILITY:        found = getLumaInstability(permutationOptions, outBlob); break;
        case FFX_FSR3UPSCALER_PASS_ACCUMULATE:
        case FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN:      found = getAccumulate(permutationOptions, outBlob); break;
        case FFX_FSR3UPSCALER_PASS_RCAS:                    found = getRcas(permutationOptions, outBlob); break;
        case FFX_FSR3UPSCALER_PASS_GENERATE_REACTIVE:       found = getAutogenReactive(permutationOptions, outBlob); break;
        case FFX_FSR3UPSCALER_PASS_DEBUG_VIEW:              found = getDebugView(permutationOptions, outBlob); break;
        default: break;
    }
    if (found)
        return FFX_OK;

    // return an empty blob
    memset(outBlob, 0, sizeof(FfxShaderBlob));
    return FFX_ERROR_INVALID_ARGUMENT;
}

FfxErrorCode fsr3UpscalerIsWave64(uint32_t permutationOptions, bool& isWave64)
{
    (void)permutationOptions;
    isWave64 = false;   // no wave64 tables in this build
    return FFX_OK;
}
