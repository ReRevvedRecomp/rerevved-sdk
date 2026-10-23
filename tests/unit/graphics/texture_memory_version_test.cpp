#include <catch2/catch_test_macros.hpp>

#include <functional>

#include <rex/graphics/pipeline/texture/cache.h>
#include "test_memory.h"

namespace {
using namespace rex::graphics;

class TestSharedMemory final : public SharedMemory {
 public:
  explicit TestSharedMemory(rex::memory::Memory& memory) : SharedMemory(memory) {
    InitializeCommon();
  }
  using SharedMemory::MakeRangeValid;
  std::function<void()> on_upload;

 private:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& ranges) override {
    if (!on_upload) {
      return false;
    }
    const auto page = uint32_t(rex::memory::page_size());
    for (const auto& [first_page, page_count] : ranges) {
      MakeRangeValid(first_page * page, page_count * page, false);
    }
    on_upload();
    return true;
  }
};

class TestTextureCache final : public TextureCache {
 public:
  TestTextureCache(const RegisterFile& registers, SharedMemory& memory)
      : TextureCache(registers, memory, 1, 1) {}
  using TextureCache::LoadTextureData;
  using TextureCache::TextureKey;
  bool load_succeeds = false;
  uint32_t load_count = 0;
  class TestTexture final : public Texture {
   public:
    TestTexture(TextureCache& cache, const TextureKey& key) : Texture(cache, key, false) {}
  };

 private:
  uint32_t GetHostFormatSwizzle(TextureKey) const override { return 0; }
  uint32_t GetMaxHostTextureWidthHeight(xenos::DataDimension) const override { return 8192; }
  uint32_t GetMaxHostTextureDepthOrArraySize(xenos::DataDimension) const override { return 2048; }
  std::unique_ptr<Texture> CreateTexture(TextureKey) override { return nullptr; }
  bool LoadTextureDataFromResidentMemoryImpl(Texture&, bool, bool) override {
    ++load_count;
    return load_succeeds;
  }
};
}  // namespace

TEST_CASE("Texture memory versions follow watched writes and rearming", "[graphics][memory]") {
  auto& memory = rex::testing::GetTestMemory();
  auto* heap = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0xA0000000));
  REQUIRE(heap);
  const auto page = uint32_t(rex::memory::page_size());
  uint32_t guest_address = 0;
  REQUIRE(heap->Alloc(
      page * 2, page, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &guest_address));
  const uint32_t address = guest_address & 0x1FFFFFFF;
  {
    TestSharedMemory shared(memory);
    RegisterFile registers;
    TestTextureCache cache(registers, shared);
    TestTextureCache::TextureKey key;
    key.base_page = address >> 12;
    key.mip_page = (address + page) >> 12;
    key.dimension = xenos::DataDimension::k2DOrStacked;
    key.width_minus_1 = 3;
    key.height_minus_1 = 3;
    key.pitch = 1;
    key.mip_max_level = 1;
    key.format = xenos::TextureFormat::k_8_8_8_8;
    key.is_valid = 1;
    TestTextureCache::TestTexture texture(cache, key);
    REQUIRE(texture.GetGuestBaseSize() <= page);
    REQUIRE(texture.GetGuestMipsSize() <= page);
    CHECK_FALSE(texture.GetGuestMemoryVersion().IsCurrent());
    rex::thread::global_critical_region region;
    const auto arm = [&] {
      shared.MakeRangeValid(address, page * 2, false);
      auto lock = region.Acquire();
      texture.MakeUpToDateAndWatch(lock, true, true);
    };
    arm();
    auto initial = texture.GetGuestMemoryVersion();
    REQUIRE(initial.IsCurrent());
    CHECK(initial.base_address == address);
    CHECK(initial.mip_address == address + page);
    CHECK(initial.base_size == texture.GetGuestBaseSize());
    CHECK(initial.mip_size == texture.GetGuestMipsSize());

    *memory.TranslateVirtual<volatile uint32_t*>(guest_address) = 1;
    CHECK_FALSE(initial.IsCurrent());
    CHECK_FALSE(texture.GetGuestMemoryVersion().IsCurrent());
    *memory.TranslateVirtual<volatile uint32_t*>(guest_address) = 2;
    CHECK_FALSE(texture.GetGuestMemoryVersion().IsCurrent());
    arm();
    auto after_base = texture.GetGuestMemoryVersion();
    REQUIRE(after_base.IsCurrent());
    CHECK(after_base.identity == initial.identity);
    CHECK(after_base.generation != initial.generation);
    CHECK_FALSE(initial.IsCurrent());

    *memory.TranslateVirtual<volatile uint32_t*>(guest_address + page) = 3;
    CHECK_FALSE(after_base.IsCurrent());
    arm();
    auto after_mips = texture.GetGuestMemoryVersion();
    REQUIRE(after_mips.IsCurrent());
    CHECK(after_mips.generation != after_base.generation);
    shared.RangeWrittenByGpu(address, page);
    CHECK_FALSE(after_mips.IsCurrent());
    // A CPU overwrite must invalidate the GPU-owned range before reuse.
    *memory.TranslateVirtual<volatile uint32_t*>(guest_address) = 4;
    arm();
    auto before_failure = texture.GetGuestMemoryVersion();
    REQUIRE(before_failure.IsCurrent());
    {
      auto lock = region.Acquire();
      texture.RestoreOutdatedAfterLoadFailure(lock, true, true);
    }
    CHECK_FALSE(before_failure.IsCurrent());
    arm();
    CHECK(texture.GetGuestMemoryVersion().IsCurrent());
    CHECK_FALSE(before_failure.IsCurrent());
    TestTextureCache::TestTexture recreated(cache, key);
    CHECK(recreated.GetGuestMemoryVersion().identity != initial.identity);
    texture.SetForceLoad3DTiling(true);
    CHECK_FALSE(texture.GetGuestMemoryVersion().IsCurrent());
    key.scaled_resolve = 1;
    TestTextureCache::TestTexture scaled(cache, key);
    CHECK_FALSE(scaled.GetGuestMemoryVersion().IsCurrent());
  }
  REQUIRE(heap->Release(guest_address, nullptr));
}

TEST_CASE("Texture loading preserves guest writes during shared memory upload",
          "[graphics][memory]") {
  auto& memory = rex::testing::GetTestMemory();
  auto* heap = const_cast<rex::memory::BaseHeap*>(memory.LookupHeap(0xA0000000));
  REQUIRE(heap);
  const auto page = uint32_t(rex::memory::page_size());
  uint32_t guest_address = 0;
  REQUIRE(heap->Alloc(
      page * 2, page, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &guest_address));
  struct Allocation {
    rex::memory::BaseHeap& heap;
    uint32_t address;
    ~Allocation() { heap.Release(address, nullptr); }
  } allocation{*heap, guest_address};
  const uint32_t address = guest_address & 0x1FFFFFFF;
  for (uint32_t write_offset : {0u, page}) {
    CAPTURE(write_offset);
    TestSharedMemory shared(memory);
    RegisterFile registers;
    TestTextureCache cache(registers, shared);
    cache.load_succeeds = true;
    TestTextureCache::TextureKey key;
    key.base_page = address >> 12;
    key.mip_page = (address + page) >> 12;
    key.dimension = xenos::DataDimension::k2DOrStacked;
    key.width_minus_1 = 3;
    key.height_minus_1 = 3;
    key.pitch = 1;
    key.mip_max_level = 1;
    key.format = xenos::TextureFormat::k_8_8_8_8;
    key.is_valid = 1;
    TestTextureCache::TestTexture texture(cache, key);
    REQUIRE(texture.GetGuestBaseSize() <= page);
    REQUIRE(texture.GetGuestMipsSize() <= page);
    diagnostic::DrawCaptureGuestMemoryVersion before_write;
    uint32_t upload_count = 0;
    shared.on_upload = [&] {
      ++upload_count;
      before_write = texture.GetGuestMemoryVersion();
      CHECK(before_write.IsCurrent());
      *memory.TranslateVirtual<volatile uint32_t*>(guest_address + write_offset) = 1;
    };

    REQUIRE(cache.LoadTextureData(texture));
    REQUIRE(upload_count == 1);
    CHECK(cache.load_count == 1);
    CHECK_FALSE(before_write.IsCurrent());
    CHECK_FALSE(texture.GetGuestMemoryVersion().IsCurrent());

    shared.on_upload = [] {};
    REQUIRE(cache.LoadTextureData(texture));
    CHECK(cache.load_count == 2);
    CHECK(texture.GetGuestMemoryVersion().IsCurrent());
    CHECK_FALSE(before_write.IsCurrent());
  }
}
