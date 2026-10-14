// Wireframe edges bypass the game's texture, alpha and face-culling rules.
#include "shared/rt64_raster_params.h"
#include "shared/rt64_render_indices.h"

[[vk::push_constant]] ConstantBuffer<RasterParams> gConstants : register(b0, space0);
StructuredBuffer<RenderIndices> instanceRenderIndices : register(t5, space0);

float4 PSMain() : SV_TARGET {
    uint color = instanceRenderIndices[gConstants.renderIndex].highlightColor;
    return float4((color >> 24) & 0xFF, (color >> 16) & 0xFF, (color >> 8) & 0xFF, 255) / 255.0f;
}
