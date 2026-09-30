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

// TF2FG PATCH (A1): accessor rewritten for the tf2-fsr-fg build.
//   - Only the Wave32 non-16bit tables exist (shaders/build_shaders.py builds one permutation per pass:
//     LOW_RES_MOTION_VECTORS=1, JITTER_MOTION_VECTORS=0, INVERTED_DEPTH=0). The wave64 / 16bit includes are gone;
//     the FORCE_WAVE64 / ALLOW_FP16 option bits never select a different table (the patched backend reports no
//     wave ops and no FP16, so they are never set anyway).
//   - A permutation that was not built (IndirectionTable entry -1) or an out-of-range key returns an EMPTY blob and
//     an error; the patched CreatePipelineDX11 then fails pipeline creation instead of running the wrong shader.
//   - An unknown pass returns an error instead of FFX_ASSERT_FAIL.

#define FFX_CPU
#include <FidelityFX/host/ffx_util.h>
#include "ffx_frameinterpolation_shaderblobs.h"
#include "frameinterpolation/ffx_frameinterpolation_private.h"

#include <ffx_frameinterpolation_reconstruct_and_dilate_pass_permutations.h>
#include <ffx_frameinterpolation_disocclusion_mask_pass_permutations.h>
#include <ffx_frameinterpolation_reconstruct_previous_depth_pass_permutations.h>
#include <ffx_frameinterpolation_setup_pass_permutations.h>
#include <ffx_frameinterpolation_game_motion_vector_field_pass_permutations.h>
#include <ffx_frameinterpolation_optical_flow_vector_field_pass_permutations.h>
#include <ffx_frameinterpolation_pass_permutations.h>
#include <ffx_frameinterpolation_compute_game_vector_field_inpainting_pyramid_pass_permutations.h>
#include <ffx_frameinterpolation_compute_inpainting_pyramid_pass_permutations.h>
#include <ffx_frameinterpolation_inpainting_pass_permutations.h>
#include <ffx_frameinterpolation_debug_view_pass_permutations.h>

#include <string.h> // for memset

#if defined(POPULATE_PERMUTATION_KEY)
#undef POPULATE_PERMUTATION_KEY
#endif // #if defined(POPULATE_PERMUTATION_KEY)
#define POPULATE_PERMUTATION_KEY(options, key)                                                                \
    key.index = 0;\
    key.FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS    = FFX_CONTAINS_FLAG(options, FRAMEINTERPOLATION_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS); \
    key.FFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS     = FFX_CONTAINS_FLAG(options, FRAMEINTERPOLATION_SHADER_PERMUTATION_JITTER_MOTION_VECTORS);  \
    key.FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH            = FFX_CONTAINS_FLAG(options, FRAMEINTERPOLATION_SHADER_PERMUTATION_DEPTH_INVERTED);

// One getter per pass: key -> IndirectionTable -> PermutationInfo, with every index checked.
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

TF2FG_DEFINE_BLOB_GETTER(getReconstructAndDilate, ffx_frameinterpolation_reconstruct_and_dilate_pass)
TF2FG_DEFINE_BLOB_GETTER(getSetup, ffx_frameinterpolation_setup_pass)
TF2FG_DEFINE_BLOB_GETTER(getReconstructPrevDepth, ffx_frameinterpolation_reconstruct_previous_depth_pass)
TF2FG_DEFINE_BLOB_GETTER(getGameMotionVectorField, ffx_frameinterpolation_game_motion_vector_field_pass)
TF2FG_DEFINE_BLOB_GETTER(getOpticalFlowVectorField, ffx_frameinterpolation_optical_flow_vector_field_pass)
TF2FG_DEFINE_BLOB_GETTER(getDisocclusionMask, ffx_frameinterpolation_disocclusion_mask_pass)
TF2FG_DEFINE_BLOB_GETTER(getInterpolation, ffx_frameinterpolation_pass)
TF2FG_DEFINE_BLOB_GETTER(getInpaintingPyramid, ffx_frameinterpolation_compute_inpainting_pyramid_pass)
TF2FG_DEFINE_BLOB_GETTER(getInpainting, ffx_frameinterpolation_inpainting_pass)
TF2FG_DEFINE_BLOB_GETTER(getGameVectorFieldInpaintingPyramid, ffx_frameinterpolation_compute_game_vector_field_inpainting_pyramid_pass)
TF2FG_DEFINE_BLOB_GETTER(getDebugView, ffx_frameinterpolation_debug_view_pass)

FfxErrorCode frameInterpolationGetPermutationBlobByIndex(FfxFrameInterpolationPass passId,
    FfxBindStage,
    uint32_t permutationOptions, FfxShaderBlob* outBlob)
{
    if (!outBlob)
        return FFX_ERROR_INVALID_POINTER;

    bool found = false;
    switch (passId) {
        case FFX_FRAMEINTERPOLATION_PASS_RECONSTRUCT_AND_DILATE:               found = getReconstructAndDilate(permutationOptions, outBlob); break;
        case FFX_FRAMEINTERPOLATION_PASS_SETUP:                                found = getSetup(permutationOptions, outBlob); break;
        case FFX_FRAMEINTERPOLATION_PASS_RECONSTRUCT_PREV_DEPTH:               found = getReconstructPrevDepth(permutationOptions, outBlob); break;
        case FFX_FRAMEINTERPOLATION_PASS_GAME_MOTION_VECTOR_FIELD:             found = getGameMotionVectorField(permutationOptions, outBlob); break;
        case FFX_FRAMEINTERPOLATION_PASS_OPTICAL_FLOW_VECTOR_FIELD:            found = getOpticalFlowVectorField(permutationOptions, outBlob); break;
        case FFX_FRAMEINTERPOLATION_PASS_DISOCCLUSION_MASK:                    found = getDisocclusionMask(permutationOptions, outBlob); break;
        case FFX_FRAMEINTERPOLATION_PASS_GAME_VECTOR_FIELD_INPAINTING_PYRAMID: found = getGameVectorFieldInpaintingPyramid(permutationOptions, outBlob); break;
        case FFX_FRAMEINTERPOLATION_PASS_INPAINTING_PYRAMID:                   found = getInpaintingPyramid(permutationOptions, outBlob); break;
        case FFX_FRAMEINTERPOLATION_PASS_INTERPOLATION:                        found = getInterpolation(permutationOptions, outBlob); break;
        case FFX_FRAMEINTERPOLATION_PASS_INPAINTING:                           found = getInpainting(permutationOptions, outBlob); break;
        case FFX_FRAMEINTERPOLATION_PASS_DEBUG_VIEW:                           found = getDebugView(permutationOptions, outBlob); break;
        default: break;
    }
    if (found)
        return FFX_OK;

    // return an empty blob
    memset(outBlob, 0, sizeof(FfxShaderBlob));
    return FFX_ERROR_INVALID_ARGUMENT;
}

FfxErrorCode frameInterpolationIsWave64(uint32_t permutationOptions, bool& isWave64)
{
    (void)permutationOptions;
    isWave64 = false;   // no wave64 tables in this build
    return FFX_OK;
}
