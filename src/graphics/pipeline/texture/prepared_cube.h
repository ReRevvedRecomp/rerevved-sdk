#pragma once

#include <rex/graphics/xenos.h>

namespace rex::graphics {

// Exact packed linear 64x64 RGBA8/RGBA16F and 128x128 BC3 CPU cube tuples shared by admission and
// demand normalization.
bool IsPreparedCubeFetch(const xenos::xe_gpu_texture_fetch_t& fetch);

}  // namespace rex::graphics
