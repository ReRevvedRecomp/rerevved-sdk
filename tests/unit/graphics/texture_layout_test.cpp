#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/pipeline/texture/util.h>

namespace {
using namespace rex::graphics;

texture_util::TextureGuestLayout StackedLayout(uint32_t layers) {
  // 128x256 tiled k_8_8_8_8 with packed mips, as in upstream issue 543.
  return texture_util::GetGuestTextureLayout(xenos::DataDimension::k2DOrStacked, 4, 128, 256,
                                             layers, true, xenos::TextureFormat::k_8_8_8_8, true,
                                             true, 8);
}
}  // namespace

TEST_CASE("stacked texture mip offsets advance by the layer count rounded up to 4", "[graphics]") {
  // Per-slice strides are 0x8000 at level 1, 0x2000 at level 2.
  for (uint32_t layers : {5u, 6u}) {
    const auto layout = StackedLayout(layers);
    REQUIRE(layout.packed_level == 3);
    CHECK(layout.mip_offsets_bytes[1] == 0);
    CHECK(layout.mip_offsets_bytes[2] == 0x40000);
    CHECK(layout.mip_offsets_bytes[3] == 0x50000);
  }
  const auto three = StackedLayout(3);
  CHECK(three.mip_offsets_bytes[2] == 0x20000);
  CHECK(three.mip_offsets_bytes[3] == 0x28000);
}

TEST_CASE("stacked texture mip offsets are unchanged for multiples of 4 and single layers",
          "[graphics]") {
  const auto four = StackedLayout(4);
  CHECK(four.mip_offsets_bytes[2] == 0x20000);
  CHECK(four.mip_offsets_bytes[3] == 0x28000);
  const auto one = StackedLayout(1);
  CHECK(one.mip_offsets_bytes[2] == 0x8000);
  CHECK(one.mip_offsets_bytes[3] == 0xA000);
}
