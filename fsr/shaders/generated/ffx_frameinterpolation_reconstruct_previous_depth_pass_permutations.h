#include "ffx_frameinterpolation_reconstruct_previous_depth_pass_b1e3dde32f37877224e75b9aa8c89625.h"

typedef union ffx_frameinterpolation_reconstruct_previous_depth_pass_PermutationKey {
    struct {
        uint32_t FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS : 1;
        uint32_t FFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS : 1;
        uint32_t FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH : 1;
    };
    uint32_t index;
} ffx_frameinterpolation_reconstruct_previous_depth_pass_PermutationKey;

typedef struct ffx_frameinterpolation_reconstruct_previous_depth_pass_PermutationInfo {
    const uint32_t       blobSize;
    const unsigned char* blobData;

    const uint32_t  numConstantBuffers;
    const char**    constantBufferNames;
    const uint32_t* constantBufferBindings;
    const uint32_t* constantBufferCounts;
    const uint32_t* constantBufferSpaces;

    const uint32_t  numSRVTextures;
    const char**    srvTextureNames;
    const uint32_t* srvTextureBindings;
    const uint32_t* srvTextureCounts;
    const uint32_t* srvTextureSpaces;

    const uint32_t  numUAVTextures;
    const char**    uavTextureNames;
    const uint32_t* uavTextureBindings;
    const uint32_t* uavTextureCounts;
    const uint32_t* uavTextureSpaces;

    const uint32_t  numSRVBuffers;
    const char**    srvBufferNames;
    const uint32_t* srvBufferBindings;
    const uint32_t* srvBufferCounts;
    const uint32_t* srvBufferSpaces;

    const uint32_t  numUAVBuffers;
    const char**    uavBufferNames;
    const uint32_t* uavBufferBindings;
    const uint32_t* uavBufferCounts;
    const uint32_t* uavBufferSpaces;

    const uint32_t  numSamplers;
    const char**    samplerNames;
    const uint32_t* samplerBindings;
    const uint32_t* samplerCounts;
    const uint32_t* samplerSpaces;

    const uint32_t  numRTAccelerationStructures;
    const char**    rtAccelerationStructureNames;
    const uint32_t* rtAccelerationStructureBindings;
    const uint32_t* rtAccelerationStructureCounts;
    const uint32_t* rtAccelerationStructureSpaces;

} ffx_frameinterpolation_reconstruct_previous_depth_pass_PermutationInfo;

// only key 1 (FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS=1, FFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS=0, FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH=0) is compiled; -1 = permutation not built
static const int32_t g_ffx_frameinterpolation_reconstruct_previous_depth_pass_IndirectionTable[] = {
    -1,
    0,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
};

static const uint32_t g_ffx_frameinterpolation_reconstruct_previous_depth_pass_PermutationCount = 1;

static const ffx_frameinterpolation_reconstruct_previous_depth_pass_PermutationInfo g_ffx_frameinterpolation_reconstruct_previous_depth_pass_PermutationInfo[] = {
    { g_ffx_frameinterpolation_reconstruct_previous_depth_pass_b1e3dde32f37877224e75b9aa8c89625_size, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_b1e3dde32f37877224e75b9aa8c89625_data, 1, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_b1e3dde32f37877224e75b9aa8c89625_CBVResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_b1e3dde32f37877224e75b9aa8c89625_CBVResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_b1e3dde32f37877224e75b9aa8c89625_CBVResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_b1e3dde32f37877224e75b9aa8c89625_CBVResourceSpaces, 2, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_b1e3dde32f37877224e75b9aa8c89625_TextureSRVResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_b1e3dde32f37877224e75b9aa8c89625_TextureSRVResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_b1e3dde32f37877224e75b9aa8c89625_TextureSRVResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_b1e3dde32f37877224e75b9aa8c89625_TextureSRVResourceSpaces, 1, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_b1e3dde32f37877224e75b9aa8c89625_TextureUAVResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_b1e3dde32f37877224e75b9aa8c89625_TextureUAVResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_b1e3dde32f37877224e75b9aa8c89625_TextureUAVResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_b1e3dde32f37877224e75b9aa8c89625_TextureUAVResourceSpaces, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};
