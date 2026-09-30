#include "ffx_opticalflow_compute_optical_flow_advanced_pass_v5_a722333da23bcb6925e9350498298961.h"

typedef union ffx_opticalflow_compute_optical_flow_advanced_pass_v5_PermutationKey {
    struct {
        uint32_t FFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT : 1;
    };
    uint32_t index;
} ffx_opticalflow_compute_optical_flow_advanced_pass_v5_PermutationKey;

typedef struct ffx_opticalflow_compute_optical_flow_advanced_pass_v5_PermutationInfo {
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

} ffx_opticalflow_compute_optical_flow_advanced_pass_v5_PermutationInfo;

// only key 0 (FFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT=0) is compiled; -1 = permutation not built
static const int32_t g_ffx_opticalflow_compute_optical_flow_advanced_pass_v5_IndirectionTable[] = {
    0,
    -1,
};

static const uint32_t g_ffx_opticalflow_compute_optical_flow_advanced_pass_v5_PermutationCount = 1;

static const ffx_opticalflow_compute_optical_flow_advanced_pass_v5_PermutationInfo g_ffx_opticalflow_compute_optical_flow_advanced_pass_v5_PermutationInfo[] = {
    { g_ffx_opticalflow_compute_optical_flow_advanced_pass_v5_a722333da23bcb6925e9350498298961_size, g_ffx_opticalflow_compute_optical_flow_advanced_pass_v5_a722333da23bcb6925e9350498298961_data, 1, g_ffx_opticalflow_compute_optical_flow_advanced_pass_v5_a722333da23bcb6925e9350498298961_CBVResourceNames, g_ffx_opticalflow_compute_optical_flow_advanced_pass_v5_a722333da23bcb6925e9350498298961_CBVResourceBindings, g_ffx_opticalflow_compute_optical_flow_advanced_pass_v5_a722333da23bcb6925e9350498298961_CBVResourceCounts, g_ffx_opticalflow_compute_optical_flow_advanced_pass_v5_a722333da23bcb6925e9350498298961_CBVResourceSpaces, 2, g_ffx_opticalflow_compute_optical_flow_advanced_pass_v5_a722333da23bcb6925e9350498298961_TextureSRVResourceNames, g_ffx_opticalflow_compute_optical_flow_advanced_pass_v5_a722333da23bcb6925e9350498298961_TextureSRVResourceBindings, g_ffx_opticalflow_compute_optical_flow_advanced_pass_v5_a722333da23bcb6925e9350498298961_TextureSRVResourceCounts, g_ffx_opticalflow_compute_optical_flow_advanced_pass_v5_a722333da23bcb6925e9350498298961_TextureSRVResourceSpaces, 2, g_ffx_opticalflow_compute_optical_flow_advanced_pass_v5_a722333da23bcb6925e9350498298961_TextureUAVResourceNames, g_ffx_opticalflow_compute_optical_flow_advanced_pass_v5_a722333da23bcb6925e9350498298961_TextureUAVResourceBindings, g_ffx_opticalflow_compute_optical_flow_advanced_pass_v5_a722333da23bcb6925e9350498298961_TextureUAVResourceCounts, g_ffx_opticalflow_compute_optical_flow_advanced_pass_v5_a722333da23bcb6925e9350498298961_TextureUAVResourceSpaces, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};
