#include "ffx_opticalflow_scale_optical_flow_advanced_pass_v5_646a0a9a53336066a873b0334e294ff9.h"

typedef union ffx_opticalflow_scale_optical_flow_advanced_pass_v5_PermutationKey {
    struct {
        uint32_t FFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT : 1;
    };
    uint32_t index;
} ffx_opticalflow_scale_optical_flow_advanced_pass_v5_PermutationKey;

typedef struct ffx_opticalflow_scale_optical_flow_advanced_pass_v5_PermutationInfo {
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

} ffx_opticalflow_scale_optical_flow_advanced_pass_v5_PermutationInfo;

// only key 0 (FFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT=0) is compiled; -1 = permutation not built
static const int32_t g_ffx_opticalflow_scale_optical_flow_advanced_pass_v5_IndirectionTable[] = {
    0,
    -1,
};

static const uint32_t g_ffx_opticalflow_scale_optical_flow_advanced_pass_v5_PermutationCount = 1;

static const ffx_opticalflow_scale_optical_flow_advanced_pass_v5_PermutationInfo g_ffx_opticalflow_scale_optical_flow_advanced_pass_v5_PermutationInfo[] = {
    { g_ffx_opticalflow_scale_optical_flow_advanced_pass_v5_646a0a9a53336066a873b0334e294ff9_size, g_ffx_opticalflow_scale_optical_flow_advanced_pass_v5_646a0a9a53336066a873b0334e294ff9_data, 1, g_ffx_opticalflow_scale_optical_flow_advanced_pass_v5_646a0a9a53336066a873b0334e294ff9_CBVResourceNames, g_ffx_opticalflow_scale_optical_flow_advanced_pass_v5_646a0a9a53336066a873b0334e294ff9_CBVResourceBindings, g_ffx_opticalflow_scale_optical_flow_advanced_pass_v5_646a0a9a53336066a873b0334e294ff9_CBVResourceCounts, g_ffx_opticalflow_scale_optical_flow_advanced_pass_v5_646a0a9a53336066a873b0334e294ff9_CBVResourceSpaces, 3, g_ffx_opticalflow_scale_optical_flow_advanced_pass_v5_646a0a9a53336066a873b0334e294ff9_TextureSRVResourceNames, g_ffx_opticalflow_scale_optical_flow_advanced_pass_v5_646a0a9a53336066a873b0334e294ff9_TextureSRVResourceBindings, g_ffx_opticalflow_scale_optical_flow_advanced_pass_v5_646a0a9a53336066a873b0334e294ff9_TextureSRVResourceCounts, g_ffx_opticalflow_scale_optical_flow_advanced_pass_v5_646a0a9a53336066a873b0334e294ff9_TextureSRVResourceSpaces, 2, g_ffx_opticalflow_scale_optical_flow_advanced_pass_v5_646a0a9a53336066a873b0334e294ff9_TextureUAVResourceNames, g_ffx_opticalflow_scale_optical_flow_advanced_pass_v5_646a0a9a53336066a873b0334e294ff9_TextureUAVResourceBindings, g_ffx_opticalflow_scale_optical_flow_advanced_pass_v5_646a0a9a53336066a873b0334e294ff9_TextureUAVResourceCounts, g_ffx_opticalflow_scale_optical_flow_advanced_pass_v5_646a0a9a53336066a873b0334e294ff9_TextureUAVResourceSpaces, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};
