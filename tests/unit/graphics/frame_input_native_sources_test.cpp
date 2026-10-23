#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <memory>
#include <cstring>
#include <tuple>
#include <vector>

#include <rex/graphics/pipeline/texture/util.h>

#include "graphics/frame_input_native_sources.h"
#include "graphics/d3d12/frame_input_copy_plan.h"
#include "prepared_frame_input_test_support.h"
#include "test_memory.h"

namespace {
using rex::graphics::FrameInputNativeSourceRejections;
using rex::graphics::FrameInputNativeSources;
using rex::graphics::SharedMemory;
using rex::graphics::testing::TestSharedMemory;
using State = FrameInputNativeSources::State;
using Composition = FrameInputNativeSources::Composition;

constexpr uint32_t kRgba8Size = 0x400000;
constexpr uint32_t kRg16FloatSize = 0x398000;
constexpr uint32_t kHalfSize = 0x384000;
constexpr uint32_t kRightOffset = 0x14000;
constexpr uint32_t kWritable = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;

struct NativeSourceFixture {
  rex::memory::Memory& memory = rex::testing::GetTestMemory();
  TestSharedMemory shared{memory};
  rex::memory::BaseHeap* heap = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0xA0000000));
  uint32_t guest = 0;
  uint32_t address = 0;
  uint64_t next_copy_id = 41;
  const uint32_t page = uint32_t(rex::memory::page_size());

  NativeSourceFixture() {
    REQUIRE(heap);
    REQUIRE(
        heap->Alloc(kRgba8Size, page,
                    rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                    kWritable, false, &guest));
    address = memory.GetPhysicalAddress(guest);
  }

  ~NativeSourceFixture() { heap->Release(guest, nullptr); }

  void Write(uint32_t offset) { *memory.TranslateVirtual<volatile uint32_t*>(guest + offset) = 1; }

  uint64_t NextCopyId() { return next_copy_id++; }

  Composition Half(bool right) const {
    return {address,
            1280,
            1280,
            720,
            uint32_t(rex::graphics::xenos::ColorFormat::k_8_8_8_8),
            0,
            right ? 640U : 0U,
            right ? 1280U : 640U};
  }

  bool ArmHalf(FrameInputNativeSources& sources, bool right, uint64_t id) {
    const auto composition = Half(right);
    return sources.Arm(address + (right ? kRightOffset : 0), kHalfSize, id, &composition);
  }

  bool CompleteHalf(FrameInputNativeSources& sources, bool right, uint64_t id) {
    const auto base = address + (right ? kRightOffset : 0);
    shared.RangeWrittenByGpu(base, kHalfSize);
    return sources.Complete(base, kHalfSize, true, id);
  }

  uint64_t Resolve(FrameInputNativeSources& sources, uint32_t offset, uint32_t size) {
    const uint64_t generation = NextCopyId();
    REQUIRE(sources.Arm(address + offset, size, generation));
    REQUIRE(generation != 0);
    shared.RangeWrittenByGpu(address + offset, size);
    REQUIRE(sources.Complete(address + offset, size, true, generation));
    uint64_t bound = 0;
    REQUIRE(sources.Classify(address + offset, size, &bound) == State::kUnresolved);
    REQUIRE(bound == generation);
    return bound;
  }
};

// Synthetic planner setup for the recorded scene halves. The window and
// scissor values reach the destination tile origin; they are not captured
// register evidence or admission requirements.
struct SceneHalfPlan {
  static constexpr uint32_t kSceneBase = 0x1D818000;
  rex::graphics::RegisterFile registers;
  rex::graphics::d3d12::FrameInputCopyGeometry geometry;
  rex::graphics::diagnostic::DrawCaptureFrameInputCopyView copy;

  void Plan(rex::memory::Memory& memory, uint64_t id, bool right, uint32_t window_y,
            uint32_t control = 0x00100040U) {
    using namespace rex::graphics;
    constexpr uint32_t geometry_address = 0x2000;
    xenos::xe_gpu_vertex_fetch_t fetch{};
    fetch.type = xenos::FetchConstantType::kVertex;
    fetch.address = geometry_address / 4;
    fetch.size = 6;
    std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], &fetch, sizeof(fetch));
    registers[XE_GPU_REG_RB_SURFACE_INFO] = 0x0A010280U;
    registers[XE_GPU_REG_RB_COLOR_INFO] = 0;
    registers[XE_GPU_REG_RB_DEPTH_INFO] = 0x530;
    registers[XE_GPU_REG_RB_MODECONTROL] = 6;
    registers[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR] = 0x20002000U;
    registers[XE_GPU_REG_PA_SU_SC_MODE_CNTL] = 0x00010000U;
    registers[XE_GPU_REG_RB_COPY_DEST_PITCH] = 0x02D00500U;
    registers[XE_GPU_REG_RB_COPY_DEST_INFO] = 0x01000300U;
    registers[XE_GPU_REG_RB_HIZ_CLEAR] = 0xFFF;
    registers[XE_GPU_REG_RB_DEPTH_CLEAR] = 0xFFFFFF00U;
    registers[XE_GPU_REG_RB_COPY_CONTROL] = control;
    registers[XE_GPU_REG_RB_COPY_DEST_BASE] = kSceneBase + (right ? 0x14000U : 0U);
    // Guest-space scissor and window offset: the right half is shifted into
    // attachment-local space and both halves are moved down by window_y.
    registers[XE_GPU_REG_PA_SC_WINDOW_OFFSET] = (right ? 0x00007D80U : 0U) | (window_y << 16);
    registers[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL] = right ? 640U : 0U;
    registers[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = (right ? 1280U : 640U) | (720U << 16);
    const float x = right ? 640.0F : 0.0F;
    rex::graphics::d3d12::FrameInputCopyGeometry source;
    source.vertices = {x - 0.5F, -0.5F, x + 639.5F, -0.5F, x + 639.5F, 719.5F};
    source.address = geometry_address;
    source.present = true;
    copy = {};
    copy.id = id;
    copy.source_geometry.memory = {&source, &d3d12::FrameInputCopyGeometry::Read};
    d3d12::PrepareFrameInputCopyPlan(copy, registers, memory, 1, 1, false, geometry);
    copy.source_geometry.memory = {&geometry, &d3d12::FrameInputCopyGeometry::Read};
  }
};
}  // namespace

TEST_CASE("Native resolve repeats replace future bindings in order",
          "[graphics][prepared][native][generations]") {
  NativeSourceFixture fixture;
  FrameInputNativeSources sources(fixture.memory, fixture.shared);
  const auto first_draw = fixture.Resolve(sources, 0, kRg16FloatSize);
  const auto second = fixture.NextCopyId();
  REQUIRE(sources.Arm(fixture.address, kRg16FloatSize, second));
  REQUIRE(second > first_draw);
  uint64_t pending = first_draw;
  CHECK(sources.Classify(fixture.address, kRg16FloatSize, &pending) == State::kRejected);
  CHECK(pending == 0);
  CHECK_FALSE(sources.Complete(fixture.address, kRg16FloatSize - 4, true, second));
  CHECK_FALSE(sources.Complete(fixture.address + 4, kRg16FloatSize, true, second));
  CHECK_FALSE(sources.Complete(fixture.address, kRg16FloatSize, true, second + 1));
  CHECK(sources.Classify(fixture.address, kRg16FloatSize) == State::kRejected);
  CHECK_FALSE(sources.Complete(fixture.address, kRg16FloatSize, true, first_draw));
  sources.Reject(fixture.address, kRg16FloatSize, first_draw);
  fixture.shared.RangeWrittenByGpu(fixture.address, kRg16FloatSize);
  REQUIRE(sources.Complete(fixture.address, kRg16FloatSize, true, second));
  uint64_t second_draw = 0;
  CHECK(sources.Classify(fixture.address, kRg16FloatSize, &second_draw) == State::kUnresolved);
  CHECK(second_draw == second);
  CHECK(second_draw != first_draw);
  CHECK_FALSE(sources.Complete(fixture.address, kRg16FloatSize, false, first_draw));
  CHECK(sources.Classify(fixture.address, kRg16FloatSize) == State::kUnresolved);
  const auto third_draw = fixture.Resolve(sources, 0, kRg16FloatSize);
  CHECK(third_draw > second_draw);
  CHECK(second_draw == second);
}

TEST_CASE("Native Rgba8 and RG16F aliases replace whole future views",
          "[graphics][prepared][native][generations]") {
  NativeSourceFixture fixture;
  FrameInputNativeSources sources(fixture.memory, fixture.shared);
  std::array<uint32_t, 3> sizes{kRgba8Size, kRg16FloatSize, kRgba8Size};
  SECTION("Rgba8 to smaller RG16F and back") {}
  SECTION("RG16F to larger Rgba8 and back") {
    sizes = {kRg16FloatSize, kRgba8Size, kRg16FloatSize};
  }
  // The sideband records plan generations, not pixel types. Equal-sized typed
  // aliases also need a fresh producer; descriptor proof belongs to the consumer.
  SECTION("Equal-extent Rgba8 and RG16F plans still get new identities") {
    sizes = {kRg16FloatSize, kRg16FloatSize, kRg16FloatSize};
  }
  const auto first_draw = fixture.Resolve(sources, 0, sizes[0]);
  const auto second_draw = fixture.Resolve(sources, 0, sizes[1]);
  CHECK(second_draw > first_draw);
  if (sizes[0] != sizes[1]) {
    CHECK(sources.Classify(fixture.address, sizes[0]) == State::kRejected);
    CHECK(sources.Classify(fixture.address + kRg16FloatSize, kRgba8Size - kRg16FloatSize) ==
          State::kRejected);
  }
  const auto third_draw = fixture.Resolve(sources, 0, sizes[2]);
  CHECK(third_draw > second_draw);
  CHECK(first_draw < second_draw);
  CHECK(sources.Classify(fixture.address + 4, sizes[2] - 4) == State::kRejected);
}

TEST_CASE("An enclosing resolve supersedes every overlapping future range",
          "[graphics][prepared][native][generations]") {
  NativeSourceFixture fixture;
  FrameInputNativeSources sources(fixture.memory, fixture.shared);
  const auto first = fixture.Resolve(sources, fixture.page, fixture.page);
  const auto second = fixture.Resolve(sources, fixture.page * 3, fixture.page);
  const auto enclosing = fixture.Resolve(sources, 0, fixture.page * 4);
  CHECK(enclosing > second);
  CHECK(second > first);
  CHECK(sources.Classify(fixture.address + fixture.page, fixture.page) == State::kRejected);
  CHECK(sources.Classify(fixture.address + fixture.page * 3, fixture.page) == State::kRejected);
}

TEST_CASE("Crossing and interior partial resolves reject only affected ranges",
          "[graphics][prepared][native][generations]") {
  NativeSourceFixture fixture;
  FrameInputNativeSourceRejections rejections;
  FrameInputNativeSources sources(fixture.memory, fixture.shared, &rejections);
  fixture.Resolve(sources, fixture.page, fixture.page * 2);
  const auto unrelated = fixture.Resolve(sources, fixture.page * 8, 16);
  uint32_t offset = fixture.page * 2;
  uint32_t size = fixture.page * 2;
  SECTION("Crosses the end") {}
  SECTION("Crosses the start") {
    offset = 0;
  }
  SECTION("Interior subrange has no replacement anchor") {
    size = fixture.page / 2;
  }
  const auto rejected = fixture.NextCopyId();
  CHECK_FALSE(sources.Arm(fixture.address + offset, size, rejected));
  CHECK_FALSE(sources.failed());
  CHECK_FALSE(sources.Complete(fixture.address + offset, size, true, rejected));
  CHECK(sources.Classify(fixture.address + offset, size) == State::kRejected);
  CHECK(sources.Classify(fixture.address + fixture.page, fixture.page * 2) == State::kRejected);
  uint64_t bound = 0;
  CHECK(sources.Classify(fixture.address + fixture.page * 8, 16, &bound) == State::kUnresolved);
  CHECK(bound == unrelated);
  CHECK(sources.Classify(fixture.address + fixture.page * 9, 16) == State::kNone);
  CHECK_FALSE(rejections.Overlaps(fixture.address + fixture.page * 8, 16));
  CHECK(rejections.Overlaps(fixture.address + offset, size));
  fixture.Resolve(sources, 0, fixture.page * 4);
}

TEST_CASE("Invalidation distinguishes a superseded tail from the current alias",
          "[graphics][prepared][native][generations]") {
  NativeSourceFixture fixture;
  FrameInputNativeSources sources(fixture.memory, fixture.shared);
  const auto earlier_draw = fixture.Resolve(sources, 0, kRgba8Size);
  const auto current_draw = fixture.Resolve(sources, 0, kRg16FloatSize);
  SECTION("Physical CPU write") {
    fixture.Write(kRg16FloatSize);
  }
  SECTION("SharedMemory GPU ownership loss") {
    fixture.shared.RangeWrittenByGpu(fixture.address + kRg16FloatSize, kRgba8Size - kRg16FloatSize);
  }
  uint64_t bound = 0;
  CHECK(sources.Classify(fixture.address, kRg16FloatSize, &bound) == State::kUnresolved);
  CHECK(bound == current_draw);
  CHECK(sources.Classify(fixture.address, kRgba8Size) == State::kRejected);
  CHECK(sources.Classify(fixture.address + kRg16FloatSize, 4) == State::kRejected);
  fixture.Write(0);
  CHECK(sources.Classify(fixture.address, kRg16FloatSize, &bound) == State::kRejected);
  CHECK(bound == 0);
  CHECK(earlier_draw < current_draw);
  CHECK_FALSE(sources.Complete(fixture.address, kRgba8Size, true, earlier_draw));
}

TEST_CASE("Replacement checks every current overlap before publishing",
          "[graphics][prepared][native][generations]") {
  NativeSourceFixture fixture;
  FrameInputNativeSourceRejections rejections;
  FrameInputNativeSources sources(fixture.memory, fixture.shared, &rejections);
  uint32_t first_offset = 0;
  SECTION("Same-base larger plan crosses a third current entry") {}
  SECTION("Enclosing plan crosses a second current entry") {
    first_offset = fixture.page;
  }
  fixture.Resolve(sources, first_offset, fixture.page);
  uint64_t unrelated = 0;
  if (first_offset) {
    fixture.Resolve(sources, fixture.page * 3, fixture.page * 2);
    unrelated = fixture.Resolve(sources, fixture.page * 8, fixture.page);
  } else {
    unrelated = fixture.Resolve(sources, fixture.page * 8, fixture.page);
    fixture.Resolve(sources, fixture.page * 3, fixture.page * 2);
  }
  const auto rejected = fixture.NextCopyId();
  CHECK_FALSE(sources.Arm(fixture.address, fixture.page * 4, rejected));
  CHECK_FALSE(sources.failed());
  CHECK_FALSE(sources.Complete(fixture.address, fixture.page * 4, true, rejected));
  CHECK(sources.Classify(fixture.address, fixture.page * 4) == State::kRejected);
  CHECK(sources.Classify(fixture.address + first_offset, fixture.page) == State::kRejected);
  CHECK(sources.Classify(fixture.address + fixture.page * 3, fixture.page * 2) == State::kRejected);
  CHECK(sources.Classify(fixture.address + fixture.page * 4, fixture.page) == State::kRejected);
  uint64_t bound = 0;
  CHECK(sources.Classify(fixture.address + fixture.page * 8, fixture.page, &bound) ==
        State::kUnresolved);
  CHECK(bound == unrelated);
  CHECK(rejections.Overlaps(fixture.address, fixture.page * 4));
  CHECK_FALSE(rejections.Overlaps(fixture.address + fixture.page * 8, fixture.page));
}

TEST_CASE("Native generation invalidation keeps existing ownership rejections",
          "[graphics][prepared][native][generations]") {
  NativeSourceFixture fixture;
  FrameInputNativeSources sources(fixture.memory, fixture.shared);
  fixture.Resolve(sources, 0, fixture.page);
  const auto current = fixture.Resolve(sources, 0, fixture.page);
  SECTION("CPU write") {
    fixture.Write(0);
  }
  SECTION("GPU write") {
    fixture.shared.RangeWrittenByGpu(fixture.address, 4);
  }
  SECTION("SharedMemory cache loss") {
    fixture.shared.ClearCache();
  }
  SECTION("Protection loss and restoration") {
    REQUIRE(
        fixture.heap->Protect(fixture.guest, fixture.page, rex::memory::kMemoryProtectNoAccess));
    REQUIRE(fixture.heap->Protect(fixture.guest, fixture.page, kWritable));
  }
  SECTION("Decommit") {
    REQUIRE(fixture.heap->Decommit(fixture.guest, fixture.page));
  }
  CHECK(sources.Classify(fixture.address, fixture.page) == State::kRejected);
  CHECK_FALSE(sources.Complete(fixture.address, fixture.page, true, current));
}

TEST_CASE("Physical invalidation keeps neighboring generations watched",
          "[graphics][prepared][native][generations]") {
  NativeSourceFixture fixture;
  FrameInputNativeSources sources(fixture.memory, fixture.shared);
  fixture.Resolve(sources, 0, fixture.page);
  fixture.Resolve(sources, fixture.page, fixture.page);
  fixture.Write(0);
  CHECK(sources.Classify(fixture.address, fixture.page) == State::kRejected);
  CHECK(sources.Classify(fixture.address + fixture.page, fixture.page) == State::kUnresolved);
  fixture.Write(fixture.page);
  CHECK(sources.Classify(fixture.address + fixture.page, fixture.page) == State::kRejected);
}

TEST_CASE("Failed and abandoned replacements cannot revive an earlier plan",
          "[graphics][prepared][native][generations]") {
  NativeSourceFixture fixture;
  FrameInputNativeSourceRejections rejections;
  FrameInputNativeSources sources(fixture.memory, fixture.shared, &rejections);
  const auto earlier = fixture.Resolve(sources, 0, kRgba8Size);
  const auto failed = fixture.NextCopyId();
  REQUIRE(sources.Arm(fixture.address, kRg16FloatSize, failed));
  SECTION("Original resolve failure") {
    CHECK_FALSE(sources.Complete(fixture.address, kRg16FloatSize, false, failed));
    CHECK(rejections.Overlaps(fixture.address, kRg16FloatSize));
  }
  SECTION("Abandoned original") {
    sources.Reject(fixture.address, kRg16FloatSize, failed);
    CHECK(rejections.Overlaps(fixture.address, kRg16FloatSize));
  }
  SECTION("CPU invalidation before original completes") {
    fixture.Write(0);
  }
  CHECK_FALSE(sources.Complete(fixture.address, kRg16FloatSize, true, failed));
  CHECK(sources.Classify(fixture.address, kRgba8Size) == State::kRejected);
  CHECK(sources.Classify(fixture.address, kRg16FloatSize) == State::kRejected);
  const auto replacement = fixture.Resolve(sources, 0, kRg16FloatSize);
  CHECK(replacement > failed);
  CHECK(failed > earlier);
}

TEST_CASE("Retiring generations preserves rejection of superseded tails",
          "[graphics][prepared][native][generations]") {
  NativeSourceFixture fixture;
  FrameInputNativeSourceRejections rejections;
  {
    FrameInputNativeSources sources(fixture.memory, fixture.shared, &rejections);
    fixture.Resolve(sources, 0, kRgba8Size);
    fixture.Resolve(sources, 0, kRg16FloatSize);
  }
  CHECK(rejections.Overlaps(fixture.address, kRg16FloatSize));
  CHECK(rejections.Overlaps(fixture.address + kRg16FloatSize, 4));
  FrameInputNativeSources replacement(fixture.memory, fixture.shared, &rejections);
  CHECK(replacement.Classify(fixture.address, kRg16FloatSize) == State::kRejected);
  fixture.Resolve(replacement, 0, kRg16FloatSize);
  CHECK(replacement.Classify(fixture.address, kRgba8Size) == State::kRejected);
  CHECK(replacement.Classify(fixture.address + kRg16FloatSize, 4) == State::kRejected);
}

TEST_CASE("Reset and owner replacement preserve caller copy identities",
          "[graphics][prepared][native][generations]") {
  NativeSourceFixture fixture;
  FrameInputNativeSources sources(fixture.memory, fixture.shared);
  const auto old = fixture.Resolve(sources, 0, 16);
  sources.Reset();
  const uint64_t current = 7000;
  REQUIRE(sources.Arm(fixture.address, 16, current));
  CHECK(current > old);
  CHECK_FALSE(sources.Complete(fixture.address, 16, true, old));
  sources.Reject(fixture.address, 16, old);
  REQUIRE(sources.Complete(fixture.address, 16, true, current));
  uint64_t bound = 0;
  CHECK(sources.Classify(fixture.address, 16, &bound) == State::kUnresolved);
  CHECK(bound == current);
  sources.Reset();
  FrameInputNativeSources replacement(fixture.memory, fixture.shared);
  REQUIRE(replacement.Arm(fixture.address, 16, current + 1));
  REQUIRE(replacement.Complete(fixture.address, 16, true, current + 1));
  CHECK(replacement.Classify(fixture.address, 16, &bound) == State::kUnresolved);
  CHECK(bound == current + 1);
}

TEST_CASE("Repeated native resolves erase contained history before capacity checks",
          "[graphics][prepared][native][generations]") {
  NativeSourceFixture fixture;
  FrameInputNativeSourceRejections rejections;
  FrameInputNativeSources sources(fixture.memory, fixture.shared, &rejections);
  bool aliases = false;
  SECTION("Exact repeats") {}
  SECTION("Alternating Rgba8 and RG16F aliases") {
    aliases = true;
  }
  uint64_t last = 0;
  for (size_t index = 0; index < FrameInputNativeSources::kMaxRanges * 2 + 1; ++index) {
    const bool shrink = aliases && (index & 1);
    last = fixture.Resolve(sources, 0, shrink ? kRg16FloatSize : kRgba8Size);
    REQUIRE(sources.range_count() == (shrink ? 2 : 1));
  }
  CHECK(last > FrameInputNativeSources::kMaxRanges * 2);
  CHECK_FALSE(sources.failed());
  CHECK_FALSE(rejections.Overlaps(fixture.address, kRgba8Size));
}

TEST_CASE("Distinct non-contained native ranges still exhaust capacity",
          "[graphics][prepared][native][generations]") {
  NativeSourceFixture fixture;
  FrameInputNativeSourceRejections rejections;
  FrameInputNativeSources sources(fixture.memory, fixture.shared, &rejections);
  for (size_t index = 0; index < FrameInputNativeSources::kMaxRanges; ++index) {
    REQUIRE(sources.Arm(fixture.address + uint32_t(index * 32), 16, fixture.NextCopyId()));
  }
  CHECK(sources.range_count() == FrameInputNativeSources::kMaxRanges);
  CHECK_FALSE(sources.Arm(fixture.address + uint32_t(FrameInputNativeSources::kMaxRanges * 32), 16,
                          fixture.NextCopyId()));
  CHECK(sources.failed());
  CHECK(sources.Classify(fixture.address, 16) == State::kRejected);
  CHECK(sources.Classify(fixture.address + fixture.page, 16) == State::kRejected);
  CHECK(rejections.Overlaps(fixture.address + fixture.page, 16));
}

TEST_CASE("Invalid native plan ranges still fail closed",
          "[graphics][prepared][native][generations]") {
  NativeSourceFixture fixture;
  FrameInputNativeSourceRejections rejections;
  FrameInputNativeSources sources(fixture.memory, fixture.shared, &rejections);
  fixture.Resolve(sources, 0, 16);
  uint32_t address = fixture.address;
  uint32_t size = 0;
  SECTION("Empty") {}
  SECTION("Outside physical memory") {
    address = SharedMemory::kBufferSize;
    size = 1;
  }
  SECTION("Crosses physical end") {
    address = SharedMemory::kBufferSize - 1;
    size = 2;
  }
  SECTION("Overflowing extent") {
    size = std::numeric_limits<uint32_t>::max();
  }
  CHECK_FALSE(sources.Arm(address, size));
  CHECK(sources.Classify(fixture.address, 16) == State::kRejected);
  CHECK(rejections.Overlaps(fixture.address + fixture.page, 16));
}

TEST_CASE("A lone composed half never proves a member or the full destination",
          "[graphics][prepared][native][generations]") {
  NativeSourceFixture fixture;
  FrameInputNativeSources sources(fixture.memory, fixture.shared);
  const auto half_size =
      rex::graphics::texture_util::GetTiledAddressUpperBound2D(640, 720, 1280, 2);
  const uint32_t right_offset = 0x14000;
  REQUIRE(half_size == 0x384000);
  bool right = false;
  SECTION("Missing right half") {}
  SECTION("Missing left half") {
    right = true;
  }
  const auto id = fixture.NextCopyId();
  REQUIRE(fixture.ArmHalf(sources, right, id));
  REQUIRE(fixture.CompleteHalf(sources, right, id));
  CHECK(sources.Classify(fixture.address + (right ? right_offset : 0), half_size) ==
        State::kRejected);
  CHECK(sources.Classify(fixture.address, kRg16FloatSize) == State::kRejected);
}

TEST_CASE("Ordered half resolves publish across the captured draw event gap",
          "[graphics][prepared][native][generations]") {
  NativeSourceFixture fixture;
  FrameInputNativeSources sources(fixture.memory, fixture.shared);
  fixture.next_copy_id = 989106;
  const auto left_id = fixture.NextCopyId();
  REQUIRE(fixture.ArmHalf(sources, false, left_id));
  REQUIRE(fixture.CompleteHalf(sources, false, left_id));
  CHECK(sources.Classify(fixture.address, kHalfSize) == State::kRejected);
  // The command processor shares its counter across draws and copies. The
  // intervening copy here is outside color-0 native-source admission.
  for (size_t index = 0; index < 405; ++index) {
    rex::graphics::diagnostic::DrawCaptureFrameInputDrawView draw;
    draw.id = fixture.NextCopyId();
    CHECK(draw.id > left_id);
    CHECK(sources.Classify(fixture.address, kRg16FloatSize) == State::kRejected);
    if (index == 200) {
      (void)fixture.NextCopyId();
    }
  }
  const auto right_id = fixture.NextCopyId();
  REQUIRE(right_id == 989513);
  REQUIRE(fixture.ArmHalf(sources, true, right_id));
  CHECK(sources.range_count() == 1);
  CHECK(sources.Classify(fixture.address, kRg16FloatSize) == State::kRejected);
  // The right GPU write crosses the left's padded range. Its old watch must
  // already be removed, or completing the right could never publish the pair.
  REQUIRE(fixture.CompleteHalf(sources, true, right_id));
  uint64_t bound = 0;
  CHECK(sources.Classify(fixture.address, kRg16FloatSize, &bound) == State::kUnresolved);
  CHECK(bound == right_id);
  CHECK(sources.Classify(fixture.address, kHalfSize) == State::kRejected);
  CHECK(sources.Classify(fixture.address + kRightOffset, kHalfSize) == State::kRejected);
  const auto retained = bound;
  const auto next_left = fixture.NextCopyId();
  const auto next_right = fixture.NextCopyId();
  REQUIRE(fixture.ArmHalf(sources, false, next_left));
  CHECK(sources.Classify(fixture.address, kRg16FloatSize) == State::kRejected);
  REQUIRE(fixture.CompleteHalf(sources, false, next_left));
  REQUIRE(fixture.ArmHalf(sources, true, next_right));
  CHECK_FALSE(sources.Complete(fixture.address, kHalfSize, false, left_id));
  sources.Reject(fixture.address + kRightOffset, kHalfSize, right_id);
  REQUIRE(fixture.CompleteHalf(sources, true, next_right));
  CHECK(sources.Classify(fixture.address, kRg16FloatSize, &bound) == State::kUnresolved);
  CHECK(bound == next_right);
  CHECK(retained == right_id);
  CHECK(sources.range_count() == 1);
}

TEST_CASE("A paired union waits for both completion results",
          "[graphics][prepared][native][generations]") {
  NativeSourceFixture fixture;
  FrameInputNativeSources sources(fixture.memory, fixture.shared);
  const auto left_id = fixture.NextCopyId();
  fixture.next_copy_id += 406;
  const auto right_id = fixture.NextCopyId();
  REQUIRE(fixture.ArmHalf(sources, false, left_id));
  fixture.shared.RangeWrittenByGpu(fixture.address, kHalfSize);
  REQUIRE(fixture.ArmHalf(sources, true, right_id));
  REQUIRE(fixture.CompleteHalf(sources, true, right_id));
  CHECK(sources.Classify(fixture.address, kRg16FloatSize) == State::kRejected);
  REQUIRE(sources.Complete(fixture.address, kHalfSize, true, left_id));
  uint64_t bound = 0;
  CHECK(sources.Classify(fixture.address, kRg16FloatSize, &bound) == State::kUnresolved);
  CHECK(bound == right_id);
}

TEST_CASE("Half pairing finds the latest left across unrelated native resolves",
          "[graphics][prepared][native][generations]") {
  NativeSourceFixture fixture;
  FrameInputNativeSources sources(fixture.memory, fixture.shared);
  const auto stale_left = fixture.NextCopyId();
  REQUIRE(fixture.ArmHalf(sources, false, stale_left));
  REQUIRE(fixture.CompleteHalf(sources, false, stale_left));
  const auto left_id = fixture.NextCopyId();
  REQUIRE(fixture.ArmHalf(sources, false, left_id));
  bool pending = false;
  SECTION("Completed left") {
    REQUIRE(fixture.CompleteHalf(sources, false, left_id));
  }
  SECTION("Pending left") {
    fixture.shared.RangeWrittenByGpu(fixture.address, kHalfSize);
    pending = true;
  }
  const auto unrelated = fixture.Resolve(sources, kRg16FloatSize + fixture.page, fixture.page);
  fixture.next_copy_id += 405;
  const auto right_id = fixture.NextCopyId();
  REQUIRE(fixture.ArmHalf(sources, true, right_id));
  REQUIRE(fixture.CompleteHalf(sources, true, right_id));
  if (pending) {
    CHECK(sources.Classify(fixture.address, kRg16FloatSize) == State::kRejected);
    REQUIRE(sources.Complete(fixture.address, kHalfSize, true, left_id));
  }
  CHECK_FALSE(sources.Complete(fixture.address, kHalfSize, false, stale_left));
  uint64_t bound = 0;
  CHECK(sources.Classify(fixture.address, kRg16FloatSize, &bound) == State::kUnresolved);
  CHECK(bound == right_id);
  CHECK(sources.Classify(fixture.address + kRg16FloatSize + fixture.page, fixture.page, &bound) ==
        State::kUnresolved);
  CHECK(bound == unrelated);
}

TEST_CASE("A third overlapping resolve cannot reuse a left member or completed pair",
          "[graphics][prepared][native][generations]") {
  NativeSourceFixture fixture;
  FrameInputNativeSources sources(fixture.memory, fixture.shared);
  const auto left_id = fixture.NextCopyId();
  REQUIRE(fixture.ArmHalf(sources, false, left_id));
  REQUIRE(fixture.CompleteHalf(sources, false, left_id));
  SECTION("Third resolve crosses the left before the right arm") {
    const auto third_id = fixture.NextCopyId();
    CHECK_FALSE(sources.Arm(fixture.address + kHalfSize - 4, 8, third_id));
    fixture.shared.RangeWrittenByGpu(fixture.address + kHalfSize - 4, 8);
    const auto right_id = fixture.NextCopyId();
    REQUIRE(fixture.ArmHalf(sources, true, right_id));
    REQUIRE(fixture.CompleteHalf(sources, true, right_id));
  }
  SECTION("Third resolve overlaps the completed pair") {
    const auto right_id = fixture.NextCopyId();
    REQUIRE(fixture.ArmHalf(sources, true, right_id));
    REQUIRE(fixture.CompleteHalf(sources, true, right_id));
    CHECK_FALSE(sources.Arm(fixture.address + fixture.page, 16, fixture.NextCopyId()));
    CHECK_FALSE(sources.Complete(fixture.address + kRightOffset, kHalfSize, true, right_id));
  }
  CHECK_FALSE(sources.failed());
  CHECK(sources.Classify(fixture.address, kRg16FloatSize) == State::kRejected);
}

TEST_CASE("Prepared scene half plans preserve logical spans after local normalization",
          "[graphics][prepared][native][generations]") {
  using namespace rex::graphics;
  bool local_right_scissor = false;
  SECTION("Guest-space right scissor") {}
  SECTION("Equivalent attachment-local right scissor") {
    local_right_scissor = true;
  }
  NativeSourceFixture fixture;
  RegisterFile registers;
  d3d12::FrameInputCopyGeometry geometry;
  const uint32_t geometry_address = fixture.address + kRgba8Size - fixture.page;
  xenos::xe_gpu_vertex_fetch_t fetch{};
  fetch.type = xenos::FetchConstantType::kVertex;
  fetch.address = geometry_address / 4;
  fetch.size = 6;
  std::memcpy(&registers.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], &fetch, sizeof(fetch));
  registers[XE_GPU_REG_RB_SURFACE_INFO] = 0x0A010280U;
  registers[XE_GPU_REG_RB_COLOR_INFO] = 0;
  registers[XE_GPU_REG_RB_DEPTH_INFO] = 0x530;
  registers[XE_GPU_REG_RB_MODECONTROL] = 6;
  registers[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR] = 0x20002000U;
  registers[XE_GPU_REG_PA_SU_SC_MODE_CNTL] = 0x00010000U;
  registers[XE_GPU_REG_RB_COPY_DEST_PITCH] = 0x02D00500U;
  registers[XE_GPU_REG_RB_COPY_DEST_INFO] = 0x01000300U;
  registers[XE_GPU_REG_RB_HIZ_CLEAR] = 0xFFF;
  registers[XE_GPU_REG_RB_DEPTH_CLEAR] = 0xFFFFFF00U;
  registers[XE_GPU_REG_RB_COLOR_CLEAR] = 0;
  registers[XE_GPU_REG_RB_COLOR_CLEAR_LO] = 0;
  constexpr uint32_t scene_base = 0x1D818000;
  FrameInputNativeSources sources(fixture.memory, fixture.shared);
  uint64_t left_id = 0;
  uint64_t right_id = 0;
  for (bool right : {false, true}) {
    registers[XE_GPU_REG_RB_COPY_CONTROL] = right ? 0x00100040U : 0x00100340U;
    // Synthetic planner setup for the recorded guest rectangles. These right
    // window/scissor values are not captured register evidence or admission requirements.
    registers[XE_GPU_REG_PA_SC_WINDOW_OFFSET] = right ? 0x00007D80U : 0;
    registers[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL] = right ? 640U : 0;
    registers[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = (right ? 1280U : 640U) | (720U << 16);
    if (right && local_right_scissor) {
      registers[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL] = 0x80000000U;
      registers[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = 640U | (720U << 16);
    }
    registers[XE_GPU_REG_RB_COPY_DEST_BASE] = scene_base + (right ? kRightOffset : 0);
    const float x = right ? 640.0F : 0.0F;
    const std::array<float, 6> vertices{x - 0.5F, -0.5F, x + 639.5F, -0.5F, x + 639.5F, 719.5F};
    std::memcpy(fixture.memory.TranslatePhysical(geometry_address), vertices.data(),
                sizeof(vertices));
    diagnostic::DrawCaptureFrameInputCopyView copy;
    copy.id = fixture.NextCopyId();
    copy.source_geometry.memory = {
        &fixture, [](void* context, uint32_t address, std::span<uint8_t> output) noexcept {
          const auto& owner = *static_cast<NativeSourceFixture*>(context);
          if (address < owner.address ||
              uint64_t(address - owner.address) + output.size() > kRgba8Size) {
            return false;
          }
          std::memcpy(output.data(), owner.memory.TranslatePhysical(address), output.size());
          return true;
        }};
    const auto guest_memory = copy.source_geometry.memory;
    d3d12::PrepareFrameInputCopyPlan(copy, registers, fixture.memory, 1, 1, false, geometry);
    copy.source_geometry.memory = {&geometry, &d3d12::FrameInputCopyGeometry::Read};
    REQUIRE(copy.cpu_data_complete);
    CHECK(copy.source_x == 0);
    CHECK(copy.planned_address == scene_base + (right ? kRightOffset : 0));
    CHECK(copy.planned_length == kHalfSize);
    Composition composition;
    REQUIRE(FrameInputNativeSources::GetPreparedResolvePlan(copy, composition));
    CHECK(composition.base == scene_base);
    CHECK(composition.width == 1280);
    CHECK(composition.pitch == 1280);
    CHECK(composition.height == 720);
    CHECK(composition.format == uint32_t(xenos::ColorFormat::k_8_8_8_8));
    CHECK(composition.endian == 0);
    CHECK(composition.x_begin == (right ? 640 : 0));
    CHECK(composition.x_end == (right ? 1280 : 640));
    if (right) {
      for (const auto reg : {XE_GPU_REG_PA_SC_WINDOW_OFFSET, XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL,
                             XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR, XE_GPU_REG_PA_SU_SC_MODE_CNTL}) {
        const auto saved = registers[reg];
        registers[reg] = 0xFFFFFFFFU;
        CHECK(FrameInputNativeSources::GetPreparedResolvePlan(copy, composition));
        CHECK(composition.x_begin == 640);
        registers[reg] = saved;
      }
      // A non-clear plan that fails the half gate is admitted standalone, never
      // composed; a clear-enabled one is not admitted at all.
      const auto expect_standalone = [&](const auto& view) {
        CHECK(FrameInputNativeSources::GetPreparedResolvePlan(view, composition));
        CHECK(composition.width == 0);
      };
      const auto reject_register = [&](auto reg, uint32_t value) {
        const auto saved = registers[reg];
        registers[reg] = value;
        expect_standalone(copy);
        registers[reg] = saved;
      };
      registers[XE_GPU_REG_RB_COPY_CONTROL] = 0x00100340U;
      CHECK_FALSE(FrameInputNativeSources::GetPreparedResolvePlan(copy, composition));
      registers[XE_GPU_REG_RB_COPY_CONTROL] = 0x00100040U;
      reject_register(XE_GPU_REG_RB_COPY_CONTROL, 0x00100050U);
      reject_register(XE_GPU_REG_RB_COPY_DEST_INFO, 0x01000302U);
      reject_register(XE_GPU_REG_RB_SURFACE_INFO, 0x0A000280U);
      reject_register(XE_GPU_REG_RB_COPY_DEST_BASE, scene_base);
      auto wrong_range = copy;
      --wrong_range.planned_length;
      expect_standalone(wrong_range);
      auto missing_geometry = copy;
      missing_geometry.source_geometry.memory = {};
      expect_standalone(missing_geometry);
      auto shifted = copy;
      shifted.copy_dest_offset_y = 8;
      expect_standalone(shifted);
      geometry.vertices[4] -= 1.0F;
      expect_standalone(copy);
      geometry.vertices[4] += 1.0F;
      REQUIRE(FrameInputNativeSources::GetPreparedResolvePlan(copy, composition));
    }
    // Relocate only the checked physical tuple into the fixture's owned memory.
    composition.base = fixture.address;
    REQUIRE(sources.Arm(fixture.address + (right ? kRightOffset : 0), copy.planned_length, copy.id,
                        &composition));
    REQUIRE(fixture.CompleteHalf(sources, right, copy.id));
    CHECK(sources.Classify(fixture.address, kRg16FloatSize) ==
          (right ? State::kUnresolved : State::kRejected));
    (right ? right_id : left_id) = copy.id;
    auto incomplete = copy;
    incomplete.cpu_data_complete = false;
    CHECK_FALSE(FrameInputNativeSources::GetPreparedResolvePlan(incomplete, composition));
    if (!right) {
      auto standalone_clear = copy;
      standalone_clear.source_width = 1280;
      CHECK_FALSE(FrameInputNativeSources::GetPreparedResolvePlan(standalone_clear, composition));
      registers[XE_GPU_REG_RB_COLOR_CLEAR] = 1;
      diagnostic::DrawCaptureFrameInputCopyView unsupported;
      unsupported.source_geometry.memory = guest_memory;
      d3d12::PrepareFrameInputCopyPlan(unsupported, registers, fixture.memory, 1, 1, false,
                                       geometry);
      CHECK_FALSE(unsupported.cpu_data_complete);
      CHECK_FALSE(FrameInputNativeSources::GetPreparedResolvePlan(unsupported, composition));
      registers[XE_GPU_REG_RB_COLOR_CLEAR] = 0;
    }
  }
  diagnostic::DrawCaptureFrameInputTextureView view;
  view.available = false;
  view.unavailable_reason = diagnostic::DrawCaptureReason::kTextureUnresolved;
  view.base_address = fixture.address;
  view.base_size = kRg16FloatSize;
  REQUIRE(sources.BindTextureView(view));
  CHECK(view.native_source_generation == right_id);
  CHECK(view.native_source_left_generation == left_id);
  CHECK(left_id < right_id);
  fixture.Write(0);
  CHECK_FALSE(sources.BindTextureView(view));
  CHECK(view.native_source_generation == 0);
  CHECK(view.native_source_left_generation == 0);
  CHECK(view.unavailable_reason == diagnostic::DrawCaptureReason::kTextureSource);
}

TEST_CASE("Standalone non-clear native plans retain their admission behavior",
          "[graphics][prepared][native][generations]") {
  using namespace rex::graphics;
  RegisterFile registers;
  diagnostic::DrawCaptureFrameInputCopyView copy;
  copy.copy_mode = "resolve";
  copy.cpu_data_complete = true;
  copy.planned_address = 0x1000;
  copy.planned_length = kRg16FloatSize;
  copy.source_rect_present = true;
  copy.source_width = 1280;
  copy.source_height = 720;
  copy.registers = {registers.values, RegisterFile::kRegisterCount};
  registers[XE_GPU_REG_RB_COPY_DEST_PITCH] = 0x02D00500U;
  SECTION("Raw Rgba8") {
    registers[XE_GPU_REG_RB_COPY_CONTROL] = 0;
    registers[XE_GPU_REG_RB_COPY_DEST_INFO] = 0x01000302U;
  }
  SECTION("Converted Rgba8") {
    registers[XE_GPU_REG_RB_COPY_CONTROL] = 0x00100000U;
    registers[XE_GPU_REG_RB_COPY_DEST_INFO] = 0x01000302U;
  }
  SECTION("Raw RG16F") {
    registers[XE_GPU_REG_RB_COPY_CONTROL] = 0;
    registers[XE_GPU_REG_RB_COPY_DEST_INFO] = 0x00000F82U;
  }
  SECTION("Converted RG16F") {
    registers[XE_GPU_REG_RB_COPY_CONTROL] = 0x00100000U;
    registers[XE_GPU_REG_RB_COPY_DEST_INFO] = 0x00000F82U;
  }
  Composition composition;
  REQUIRE(FrameInputNativeSources::GetPreparedResolvePlan(copy, composition));
  CHECK(composition.width == 0);
  registers[XE_GPU_REG_RB_COPY_CONTROL] |= 0x300U;
  CHECK_FALSE(FrameInputNativeSources::GetPreparedResolvePlan(copy, composition));
}

TEST_CASE("Composed half keys and submission order cannot be inferred from addresses",
          "[graphics][prepared][native][generations]") {
  NativeSourceFixture fixture;
  FrameInputNativeSourceRejections rejections;
  FrameInputNativeSources sources(fixture.memory, fixture.shared, &rejections);
  fixture.Resolve(sources, kRg16FloatSize + fixture.page, fixture.page);
  auto left = fixture.Half(false);
  auto right = fixture.Half(true);
  uint64_t left_id = fixture.NextCopyId();
  uint64_t right_id = fixture.NextCopyId();
  bool reverse = false;
  SECTION("Equal copy IDs") {
    right_id = left_id;
  }
  SECTION("Decreasing copy IDs") {
    right_id = left_id - 1;
  }
  SECTION("Untagged copy IDs") {
    left_id = right_id = 0;
  }
  SECTION("Right submitted first") {
    reverse = true;
  }
  SECTION("Destination base differs") {
    right.base += fixture.page;
  }
  SECTION("Destination width differs") {
    ++right.width;
  }
  SECTION("Destination pitch differs") {
    right.pitch += 32;
  }
  SECTION("Destination height differs") {
    ++right.height;
  }
  SECTION("Destination format differs") {
    ++right.format;
  }
  SECTION("Destination endian differs") {
    ++right.endian;
  }
  SECTION("Logical spans overlap") {
    --right.x_begin;
  }
  SECTION("Logical spans have a gap") {
    ++right.x_begin;
  }
  SECTION("Logical union misses the left edge") {
    ++left.x_begin;
  }
  SECTION("Logical union misses the right edge") {
    --right.x_end;
  }
  if (reverse) {
    REQUIRE(sources.Arm(fixture.address + kRightOffset, kHalfSize, left_id, &right));
    REQUIRE(sources.Complete(fixture.address + kRightOffset, kHalfSize, true, left_id));
    CHECK_FALSE(sources.Arm(fixture.address, kHalfSize, right_id, &left));
  } else {
    REQUIRE(sources.Arm(fixture.address, kHalfSize, left_id, &left));
    REQUIRE(sources.Complete(fixture.address, kHalfSize, true, left_id));
    CHECK_FALSE(sources.Arm(fixture.address + kRightOffset, kHalfSize, right_id, &right));
  }
  CHECK_FALSE(sources.failed());
  CHECK(sources.Classify(fixture.address, kRg16FloatSize) == State::kRejected);
  CHECK(sources.Classify(fixture.address, kHalfSize) == State::kRejected);
  CHECK(sources.Classify(fixture.address + kRightOffset, kHalfSize) == State::kRejected);
  CHECK(sources.Classify(fixture.address + kRg16FloatSize + fixture.page, fixture.page) ==
        State::kUnresolved);
}

TEST_CASE("Left-half failure before the second arm rejects the composed union",
          "[graphics][prepared][native][generations]") {
  NativeSourceFixture fixture;
  FrameInputNativeSourceRejections rejections;
  FrameInputNativeSources sources(fixture.memory, fixture.shared, &rejections);
  const auto left_id = fixture.NextCopyId();
  fixture.next_copy_id += 406;
  const auto right_id = fixture.NextCopyId();
  REQUIRE(fixture.ArmHalf(sources, false, left_id));
  SECTION("Original failure") {
    CHECK_FALSE(sources.Complete(fixture.address, kHalfSize, false, left_id));
  }
  SECTION("Abandoned left") {
    sources.Reject(fixture.address, kHalfSize, left_id);
  }
  SECTION("Physical invalidation") {
    fixture.Write(0);
  }
  SECTION("GPU overwrite after left completion") {
    REQUIRE(fixture.CompleteHalf(sources, false, left_id));
    fixture.shared.RangeWrittenByGpu(fixture.address, 4);
  }
  SECTION("Crossing GPU write after left completion") {
    REQUIRE(fixture.CompleteHalf(sources, false, left_id));
    fixture.shared.RangeWrittenByGpu(fixture.address + kHalfSize - 4, 8);
  }
  SECTION("CPU write to the completed left") {
    REQUIRE(fixture.CompleteHalf(sources, false, left_id));
    fixture.Write(0);
  }
  SECTION("CPU write to the completed left tail the right will overlap") {
    REQUIRE(fixture.CompleteHalf(sources, false, left_id));
    fixture.Write(kHalfSize - 4);
  }
  CHECK_FALSE(fixture.ArmHalf(sources, true, right_id));
  CHECK_FALSE(sources.failed());
  CHECK_FALSE(sources.Complete(fixture.address + kRightOffset, kHalfSize, true, right_id));
  CHECK(sources.Classify(fixture.address, kRg16FloatSize) == State::kRejected);
  CHECK(rejections.Overlaps(fixture.address + kHalfSize, 4));
}

TEST_CASE("Either paired member failure rejects the union without reviving history",
          "[graphics][prepared][native][generations]") {
  NativeSourceFixture fixture;
  FrameInputNativeSourceRejections rejections;
  FrameInputNativeSources sources(fixture.memory, fixture.shared, &rejections);
  // Exercise the old large alias tail as well as both members of the pair.
  fixture.Resolve(sources, 0, kRgba8Size);
  const auto left_id = fixture.NextCopyId();
  const auto right_id = fixture.NextCopyId();
  REQUIRE(fixture.ArmHalf(sources, false, left_id));
  REQUIRE(fixture.CompleteHalf(sources, false, left_id));
  REQUIRE(fixture.ArmHalf(sources, true, right_id));
  SECTION("Left original fails") {
    CHECK_FALSE(sources.Complete(fixture.address, kHalfSize, false, left_id));
  }
  SECTION("Right original fails") {
    CHECK_FALSE(sources.Complete(fixture.address + kRightOffset, kHalfSize, false, right_id));
  }
  SECTION("Left abandoned") {
    sources.Reject(fixture.address, kHalfSize, left_id);
  }
  SECTION("Right abandoned") {
    sources.Reject(fixture.address + kRightOffset, kHalfSize, right_id);
  }
  SECTION("Left physical invalidation before completion") {
    fixture.Write(0);
  }
  SECTION("Right physical invalidation before completion") {
    fixture.Write(kHalfSize);
  }
  SECTION("Completed union left CPU overwrite") {
    REQUIRE(fixture.CompleteHalf(sources, true, right_id));
    fixture.Write(0);
  }
  SECTION("Completed union right CPU overwrite") {
    REQUIRE(fixture.CompleteHalf(sources, true, right_id));
    fixture.Write(kHalfSize);
  }
  SECTION("Completed union left GPU overwrite") {
    REQUIRE(fixture.CompleteHalf(sources, true, right_id));
    fixture.shared.RangeWrittenByGpu(fixture.address, 4);
  }
  SECTION("Completed union right GPU overwrite") {
    REQUIRE(fixture.CompleteHalf(sources, true, right_id));
    fixture.shared.RangeWrittenByGpu(fixture.address + kHalfSize, 4);
  }
  SECTION("Completed union SharedMemory loss") {
    REQUIRE(fixture.CompleteHalf(sources, true, right_id));
    fixture.shared.ClearCache();
  }
  CHECK_FALSE(sources.failed());
  CHECK_FALSE(sources.Complete(fixture.address + kRightOffset, kHalfSize, true, right_id));
  uint64_t bound = 999;
  CHECK(sources.Classify(fixture.address, kRg16FloatSize, &bound) == State::kRejected);
  CHECK(bound == 0);
  CHECK(sources.Classify(fixture.address, kRgba8Size) == State::kRejected);
  CHECK(rejections.Overlaps(fixture.address, 4));
  CHECK(rejections.Overlaps(fixture.address + kHalfSize, 4));
}

TEST_CASE("Half admission requires a zero planned destination origin",
          "[graphics][prepared][native][generations]") {
  // The planner reports the same rectangle, base and 32x32-tile lower bound
  // for rows 8..727 as for rows 0..719, so only the destination origin tells
  // them apart.
  for (const uint32_t window_y : {0U, 8U, 16U}) {
    for (const bool right : {false, true}) {
      INFO("window_y " << window_y << (right ? " right" : " left"));
      NativeSourceFixture fixture;
      auto plan = std::make_unique<SceneHalfPlan>();
      plan->Plan(fixture.memory, 1, right, window_y);
      const auto& copy = plan->copy;
      REQUIRE(copy.cpu_data_complete);
      CHECK(copy.source_x == 0);
      CHECK(copy.source_y == 0);
      CHECK(copy.source_width == 640);
      CHECK(copy.source_height == 720);
      CHECK(copy.planned_address == SceneHalfPlan::kSceneBase + (right ? kRightOffset : 0));
      CHECK(copy.planned_length == kHalfSize);
      CHECK(copy.copy_dest_offset_x == 0);
      CHECK(copy.copy_dest_offset_y == window_y);
      Composition composition;
      REQUIRE(FrameInputNativeSources::GetPreparedResolvePlan(copy, composition));
      // A rejected half is an ordinary standalone plan, never a union member.
      CHECK((composition.width == 1280) == (window_y == 0));
    }
  }
}

TEST_CASE("A half with a hidden destination origin cannot join a pair",
          "[graphics][prepared][native][generations]") {
  NativeSourceFixture fixture;
  FrameInputNativeSources sources(fixture.memory, fixture.shared);
  auto left = std::make_unique<SceneHalfPlan>();
  auto right = std::make_unique<SceneHalfPlan>();
  const auto left_id = fixture.NextCopyId();
  const auto right_id = fixture.NextCopyId();
  SECTION("Shifted right") {
    left->Plan(fixture.memory, left_id, false, 0);
    right->Plan(fixture.memory, right_id, true, 8);
  }
  SECTION("Shifted left") {
    left->Plan(fixture.memory, left_id, false, 8, 0x00100040U);
    right->Plan(fixture.memory, right_id, true, 0);
  }
  Composition left_plan;
  Composition right_plan;
  REQUIRE(FrameInputNativeSources::GetPreparedResolvePlan(left->copy, left_plan));
  REQUIRE(FrameInputNativeSources::GetPreparedResolvePlan(right->copy, right_plan));
  CHECK((left_plan.width == 0) != (right_plan.width == 0));
  left_plan.base = right_plan.base = fixture.address;
  REQUIRE(sources.Arm(fixture.address, left->copy.planned_length, left_id,
                      left_plan.width ? &left_plan : nullptr));
  REQUIRE(fixture.CompleteHalf(sources, false, left_id));
  // The second half overlaps the first without composing, so the first is
  // superseded and no union can be published.
  CHECK_FALSE(sources.Arm(fixture.address + kRightOffset, right->copy.planned_length, right_id,
                          right_plan.width ? &right_plan : nullptr));
  CHECK_FALSE(sources.failed());
  CHECK(sources.Classify(fixture.address, kRg16FloatSize) == State::kRejected);
  CHECK(sources.Classify(fixture.address, kHalfSize) == State::kRejected);
}

namespace {
// Hand-built prepared copy view for the half gate; no planner is involved.
struct SyntheticHalfCopy {
  rex::graphics::RegisterFile registers;
  rex::graphics::d3d12::FrameInputCopyGeometry geometry;
  rex::graphics::diagnostic::DrawCaptureFrameInputCopyView copy;
  bool right = false;

  SyntheticHalfCopy() {
    using namespace rex::graphics;
    registers[XE_GPU_REG_RB_COPY_DEST_PITCH] = 0x02D00500U;
    registers[XE_GPU_REG_RB_COPY_DEST_INFO] = 0x01000300U;
    registers[XE_GPU_REG_RB_SURFACE_INFO] = 0x0A010280U;
    geometry.address = 0x2000;
    geometry.present = true;
    copy.copy_mode = "resolve";
    copy.cpu_data_complete = true;
    copy.source_rect_present = true;
    copy.source_width = 640;
    copy.source_height = 720;
    copy.planned_length = kHalfSize;
    copy.registers = {registers.values, RegisterFile::kRegisterCount};
    copy.source_geometry.physical_address = geometry.address;
    copy.source_geometry.memory = {&geometry, &d3d12::FrameInputCopyGeometry::Read};
    Set(false, 0x00100340U);
  }

  void Set(bool is_right, uint32_t control) {
    using namespace rex::graphics;
    right = is_right;
    const float x = right ? 640.0F : 0.0F;
    geometry.vertices = {x - 0.5F, -0.5F, x + 639.5F, -0.5F, x + 639.5F, 719.5F};
    registers[XE_GPU_REG_RB_COPY_CONTROL] = control;
    registers[XE_GPU_REG_RB_COPY_DEST_BASE] =
        SceneHalfPlan::kSceneBase + (right ? kRightOffset : 0);
    copy.planned_address = registers[XE_GPU_REG_RB_COPY_DEST_BASE];
  }
};
}  // namespace

TEST_CASE("640-wide Rgba8 plans compose only as strict halves and otherwise stay standalone",
          "[graphics][prepared][native][generations]") {
  using namespace rex::graphics;
  auto half = std::make_unique<SyntheticHalfCopy>();
  auto& copy = half->copy;
  auto& registers = half->registers;
  // Default expectation: a valid clear-enabled left half composes.
  bool admitted = true;
  bool composed = true;
  SECTION("Valid left with clear") {}
  SECTION("Valid left without clear") {
    half->Set(false, 0x00100040U);
  }
  SECTION("Valid right") {
    half->Set(true, 0x00100040U);
  }
  SECTION("Raw copy is standalone") {
    half->Set(false, 0);
    registers[XE_GPU_REG_RB_COPY_DEST_INFO] = 0x01000302U;
    composed = false;
  }
  SECTION("Converted copy is standalone") {
    half->Set(true, 0x00100000U);
    registers[XE_GPU_REG_RB_COPY_DEST_INFO] = 0x01000302U;
    composed = false;
  }
  SECTION("Missing geometry is standalone") {
    half->Set(true, 0x00100040U);
    copy.source_geometry.memory = {};
    composed = false;
  }
  SECTION("Changed rectangle is standalone") {
    half->Set(true, 0x00100040U);
    half->geometry.vertices[4] -= 1.0F;
    composed = false;
  }
  SECTION("Wrong sample count is standalone") {
    half->Set(false, 0x00100040U);
    registers[XE_GPU_REG_RB_SURFACE_INFO] = 0x0A000280U;
    composed = false;
  }
  SECTION("Misplaced planned range is standalone") {
    half->Set(true, 0x00100040U);
    copy.planned_address += 4;
    composed = false;
  }
  SECTION("Right half below the adjustment is standalone") {
    half->Set(true, 0x00100040U);
    registers[XE_GPU_REG_RB_COPY_DEST_BASE] = 0x1000;
    copy.planned_address = 0x1000;
    composed = false;
  }
  SECTION("Sub-tile source offset is standalone") {
    half->Set(false, 0x00100040U);
    copy.source_y = 8;
    composed = false;
  }
  SECTION("Nonzero destination origin is standalone") {
    half->Set(true, 0x00100040U);
    copy.copy_dest_offset_y = 8;
    composed = false;
  }
  SECTION("Nonzero destination column origin is standalone") {
    half->Set(false, 0x00100040U);
    copy.copy_dest_offset_x = 8;
    composed = false;
  }
  SECTION("Clear with a nonzero destination origin is not admitted") {
    copy.copy_dest_offset_y = 8;
    admitted = composed = false;
  }
  SECTION("Clear without geometry is not admitted") {
    copy.source_geometry.memory = {};
    admitted = composed = false;
  }
  SECTION("Clear on the right half is not admitted") {
    half->Set(true, 0x00100340U);
    admitted = composed = false;
  }
  Composition composition;
  CHECK(FrameInputNativeSources::GetPreparedResolvePlan(copy, composition) == admitted);
  CHECK((composition.width == 1280) == composed);
  if (composed) {
    CHECK(composition.base == SceneHalfPlan::kSceneBase);
    CHECK(composition.x_begin == (half->right ? 640U : 0U));
    CHECK(composition.x_end == (half->right ? 1280U : 640U));
  } else {
    CHECK(composition.width == 0);
  }
}

TEST_CASE("Bound views carry both paired producer events",
          "[graphics][prepared][native][generations]") {
  using namespace rex::graphics;
  NativeSourceFixture fixture;
  FrameInputNativeSources sources(fixture.memory, fixture.shared);
  const auto standalone = fixture.Resolve(sources, kRg16FloatSize + fixture.page, fixture.page);
  const auto left_id = fixture.NextCopyId();
  fixture.next_copy_id += 405;
  const auto right_id = fixture.NextCopyId();
  REQUIRE(fixture.ArmHalf(sources, false, left_id));
  REQUIRE(fixture.CompleteHalf(sources, false, left_id));
  REQUIRE(fixture.ArmHalf(sources, true, right_id));
  REQUIRE(fixture.CompleteHalf(sources, true, right_id));
  const auto bind = [&](uint32_t address, uint32_t size) {
    diagnostic::DrawCaptureFrameInputTextureView view;
    view.available = false;
    view.unavailable_reason = diagnostic::DrawCaptureReason::kTextureUnresolved;
    view.base_address = address;
    view.base_size = size;
    // Prove the fields are overwritten rather than inherited.
    view.native_source_generation = view.native_source_left_generation = 999;
    const bool bound = sources.BindTextureView(view);
    return std::tuple(bound, view.native_source_generation, view.native_source_left_generation);
  };
  CHECK(bind(fixture.address, kRg16FloatSize) == std::tuple(true, right_id, left_id));
  CHECK(bind(fixture.address + kRg16FloatSize + fixture.page, fixture.page) ==
        std::tuple(true, standalone, uint64_t(0)));
  CHECK(bind(fixture.address, kHalfSize) == std::tuple(false, uint64_t(0), uint64_t(0)));
  fixture.Write(kHalfSize);
  CHECK(bind(fixture.address, kRg16FloatSize) == std::tuple(false, uint64_t(0), uint64_t(0)));
}

#ifdef REX_TEST_DXBC_TRANSLATION
TEST_CASE("Bound prepared texture views retain the admitted copy event identity",
          "[graphics][prepared][native][generations]") {
  rex::graphics::testing::PreparedFrameInputTestSupport support(rex::testing::GetTestMemory());
  std::string error;
  REQUIRE(support.BuildConsumerDraw(error));
  for (const auto& view : support.draw().textures) {
    CHECK(view.native_source_generation == 0);
  }
  constexpr uint64_t first_copy = 109;
  REQUIRE(support.ArmNativeSource(error, first_copy));
  REQUIRE(support.CompleteNativeSource(true, error));
  REQUIRE(support.BuildConsumerDraw(error));
  std::vector<rex::graphics::diagnostic::DrawCaptureFrameInputTextureView> earlier;
  for (const auto& view : support.draw().textures) {
    if (view.required) {
      CHECK(view.unavailable_reason ==
            rex::graphics::diagnostic::DrawCaptureReason::kTextureUnresolved);
      CHECK(view.native_source_generation == first_copy);
      earlier.push_back(view);
    }
  }
  REQUIRE_FALSE(earlier.empty());
  REQUIRE(support.ArmNativeSource(error, first_copy + 1));
  CHECK_FALSE(support.BuildConsumerDraw(error));
  for (const auto& view : support.draw().textures) {
    CHECK(view.native_source_generation == 0);
  }
  REQUIRE(support.CompleteNativeSource(true, error));
  REQUIRE(support.BuildConsumerDraw(error));
  for (const auto& view : support.draw().textures) {
    CHECK(view.native_source_generation == (view.required ? first_copy + 1 : 0));
  }
  for (const auto& view : earlier) {
    CHECK(view.native_source_generation == first_copy);
  }
}
#endif
