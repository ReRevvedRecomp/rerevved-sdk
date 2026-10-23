#pragma once

#include <cstdint>

#include <rex/graphics/d3d12/texture_cache.h>
#include <rex/graphics/register_file.h>

namespace rex::graphics::d3d12 {

// Computes the sampler value from the current guest fetch and the prepared
// sampler overrides. The caller supplies the runtime anisotropic override so
// this pure calculation can be shared by production and prepared test input.
D3D12TextureCache::SamplerParameters GetFrameInputSamplerParameters(
    const RegisterFile& registers, const D3D12Shader::SamplerBinding& binding,
    int32_t anisotropic_override);

}  // namespace rex::graphics::d3d12
