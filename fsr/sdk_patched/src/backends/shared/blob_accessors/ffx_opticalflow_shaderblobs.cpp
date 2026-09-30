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

// TF2FG PATCH (A2): accessor rewritten for the tf2-fsr-fg build (same rules as the FI accessor, A1):
// Wave32 non-16bit tables only (HDR_COLOR_INPUT=0 is the one permutation built), a permutation that was not built
// or an out-of-range key returns an empty blob + error, an unknown pass returns an error.

#include <FidelityFX/host/ffx_util.h>
#include "ffx_opticalflow_shaderblobs.h"
#include "opticalflow/ffx_opticalflow_private.h"

#include <ffx_opticalflow_compute_luminance_pyramid_pass_permutations.h>
#include <ffx_opticalflow_compute_optical_flow_advanced_pass_v5_permutations.h>
#include <ffx_opticalflow_compute_scd_divergence_pass_permutations.h>
#include <ffx_opticalflow_filter_optical_flow_pass_v5_permutations.h>
#include <ffx_opticalflow_generate_scd_histogram_pass_permutations.h>
#include <ffx_opticalflow_prepare_luma_pass_permutations.h>
#include <ffx_opticalflow_scale_optical_flow_advanced_pass_v5_permutations.h>

#include <string.h> // for memset

#if defined(POPULATE_PERMUTATION_KEY)
#undef POPULATE_PERMUTATION_KEY
#endif // #if defined(POPULATE_PERMUTATION_KEY)
#define POPULATE_PERMUTATION_KEY(options, key)   \
    key.index                                   = 0; \
    key.FFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT = FFX_CONTAINS_FLAG(options, OPTICALFLOW_HDR_COLOR_INPUT);

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

TF2FG_DEFINE_BLOB_GETTER(getPrepareLuma, ffx_opticalflow_prepare_luma_pass)
TF2FG_DEFINE_BLOB_GETTER(getLuminancePyramid, ffx_opticalflow_compute_luminance_pyramid_pass)
TF2FG_DEFINE_BLOB_GETTER(getScdHistogram, ffx_opticalflow_generate_scd_histogram_pass)
TF2FG_DEFINE_BLOB_GETTER(getScdDivergence, ffx_opticalflow_compute_scd_divergence_pass)
TF2FG_DEFINE_BLOB_GETTER(getOpticalFlowV5, ffx_opticalflow_compute_optical_flow_advanced_pass_v5)
TF2FG_DEFINE_BLOB_GETTER(getFilterV5, ffx_opticalflow_filter_optical_flow_pass_v5)
TF2FG_DEFINE_BLOB_GETTER(getScaleV5, ffx_opticalflow_scale_optical_flow_advanced_pass_v5)

FfxErrorCode opticalflowGetPermutationBlobByIndex(
    FfxOpticalflowPass passId,
    uint32_t permutationOptions,
    FfxShaderBlob* outBlob) {

    if (!outBlob)
        return FFX_ERROR_INVALID_POINTER;

    bool found = false;
    switch (passId) {
        case FFX_OPTICALFLOW_PASS_PREPARE_LUMA:                        found = getPrepareLuma(permutationOptions, outBlob); break;
        case FFX_OPTICALFLOW_PASS_GENERATE_OPTICAL_FLOW_INPUT_PYRAMID: found = getLuminancePyramid(permutationOptions, outBlob); break;
        case FFX_OPTICALFLOW_PASS_GENERATE_SCD_HISTOGRAM:              found = getScdHistogram(permutationOptions, outBlob); break;
        case FFX_OPTICALFLOW_PASS_COMPUTE_SCD_DIVERGENCE:              found = getScdDivergence(permutationOptions, outBlob); break;
        case FFX_OPTICALFLOW_PASS_COMPUTE_OPTICAL_FLOW_ADVANCED_V5:    found = getOpticalFlowV5(permutationOptions, outBlob); break;
        case FFX_OPTICALFLOW_PASS_FILTER_OPTICAL_FLOW_V5:              found = getFilterV5(permutationOptions, outBlob); break;
        case FFX_OPTICALFLOW_PASS_SCALE_OPTICAL_FLOW_ADVANCED_V5:      found = getScaleV5(permutationOptions, outBlob); break;
        default: break;
    }
    if (found)
        return FFX_OK;

    // return an empty blob
    memset(outBlob, 0, sizeof(FfxShaderBlob));
    return FFX_ERROR_INVALID_ARGUMENT;
}

FfxErrorCode opticalflowIsWave64(uint32_t permutationOptions, bool& isWave64)
{
    (void)permutationOptions;
    isWave64 = false;   // no wave64 tables in this build
    return FFX_OK;
}
