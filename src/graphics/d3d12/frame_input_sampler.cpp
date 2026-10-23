#include "frame_input_sampler.h"

#include <algorithm>

#include <rex/graphics/pipeline/texture/util.h>

namespace rex::graphics::d3d12 {
namespace {

xenos::ClampMode NormalizeClampMode(xenos::ClampMode clamp_mode) {
  if (clamp_mode == xenos::ClampMode::kClampToHalfway)
    return xenos::ClampMode::kClampToEdge;
  if (clamp_mode == xenos::ClampMode::kMirrorClampToHalfway ||
      clamp_mode == xenos::ClampMode::kMirrorClampToBorder)
    return xenos::ClampMode::kMirrorClampToEdge;
  return clamp_mode;
}

}  // namespace

D3D12TextureCache::SamplerParameters GetFrameInputSamplerParameters(
    const RegisterFile& registers, const D3D12Shader::SamplerBinding& binding,
    int32_t anisotropic_override) {
  const auto fetch = registers.GetTextureFetch(binding.fetch_constant);
  D3D12TextureCache::SamplerParameters parameters;

  xenos::ClampMode fetch_clamp_x, fetch_clamp_y, fetch_clamp_z;
  texture_util::GetClampModesForDimension(fetch, fetch_clamp_x, fetch_clamp_y, fetch_clamp_z);
  parameters.clamp_x = NormalizeClampMode(fetch_clamp_x);
  parameters.clamp_y = NormalizeClampMode(fetch_clamp_y);
  parameters.clamp_z = NormalizeClampMode(fetch_clamp_z);
  if (xenos::ClampModeUsesBorder(parameters.clamp_x) ||
      xenos::ClampModeUsesBorder(parameters.clamp_y) ||
      xenos::ClampModeUsesBorder(parameters.clamp_z)) {
    parameters.border_color = fetch.border_color;
  } else {
    parameters.border_color = xenos::BorderColor::k_ABGR_Black;
  }

  uint32_t mip_min_level, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, nullptr, nullptr,
                                                 &mip_min_level, &mip_max_level);
  parameters.mip_min_level = mip_min_level;
  const bool has_mips = mip_max_level > mip_min_level;

  const auto mag_filter = binding.mag_filter == xenos::TextureFilter::kUseFetchConst
                              ? fetch.mag_filter
                              : binding.mag_filter;
  const auto min_filter = binding.min_filter == xenos::TextureFilter::kUseFetchConst
                              ? fetch.min_filter
                              : binding.min_filter;
  const auto mip_filter = binding.mip_filter == xenos::TextureFilter::kUseFetchConst
                              ? fetch.mip_filter
                              : binding.mip_filter;
  const bool min_mag_linear =
      mag_filter == xenos::TextureFilter::kLinear && min_filter == xenos::TextureFilter::kLinear;
  const bool mip_filter_bilinear_or_trilinear =
      mip_filter == xenos::TextureFilter::kPoint || mip_filter == xenos::TextureFilter::kLinear;
  const bool mip_base_map = mip_filter == xenos::TextureFilter::kBaseMap;

  auto aniso_filter = binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst
                          ? fetch.aniso_filter
                          : binding.aniso_filter;
  if (anisotropic_override > -1 && anisotropic_override < 6 && has_mips && !mip_base_map &&
      min_mag_linear && mip_filter_bilinear_or_trilinear)
    aniso_filter = xenos::AnisoFilter(anisotropic_override);
  aniso_filter = std::min(aniso_filter, xenos::AnisoFilter::kMax_16_1);
  parameters.aniso_filter = aniso_filter;
  if (aniso_filter != xenos::AnisoFilter::kDisabled) {
    parameters.mag_linear = 1;
    parameters.min_linear = 1;
    parameters.mip_linear = 1;
  } else {
    parameters.mag_linear = mag_filter == xenos::TextureFilter::kLinear;
    parameters.min_linear = min_filter == xenos::TextureFilter::kLinear;
    parameters.mip_linear = mip_filter == xenos::TextureFilter::kLinear;
  }
  parameters.mip_base_map = mip_base_map;
  return parameters;
}

}  // namespace rex::graphics::d3d12
