/**
 * @file        guest_memory_section_test.cpp
 * @brief       Guest memory section names must not collide across mappings
 *
 * Section names are system-wide, so processes that start together (ctest -j) once shared one
 * guest memory block when they picked the same name in the same host tick.
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <cstdint>
#include <cstring>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/xmemory.h>

#include "system/memory_section_name.h"

namespace {

using rex::memory::detail::GuestSectionNameSource;
using rex::memory::detail::NextGuestSectionName;
using rex::memory::detail::SetGuestSectionNameSourceForTesting;

constexpr size_t kTestSectionSize = 1 << 20;

std::string g_taken_name;
std::string g_free_name;
int g_source_calls = 0;

// First call returns the name that is already in use, later calls a free one.
std::string TakenThenFreeSource() {
  return g_source_calls++ == 0 ? g_taken_name : g_free_name;
}

std::string AlwaysTakenSource() {
  ++g_source_calls;
  return g_taken_name;
}

class ScopedNameSource {
 public:
  explicit ScopedNameSource(GuestSectionNameSource source)
      : previous_(SetGuestSectionNameSourceForTesting(source)) {
    g_source_calls = 0;
  }
  ~ScopedNameSource() { SetGuestSectionNameSourceForTesting(previous_); }

 private:
  GuestSectionNameSource previous_;
};

rex::memory::FileMappingHandle CreateTestSection(const std::string& name) {
  return rex::memory::CreateFileMappingHandle(name, kTestSectionSize,
                                              rex::memory::PageAccess::kReadWrite, true);
}

}  // namespace

TEST_CASE("Guest memory section names are unique within a process", "[memory][section]") {
  const std::string first = NextGuestSectionName();
  const std::string second = NextGuestSectionName();
  CHECK(first != second);
  // QueryPerformanceCounter ticks can repeat; the per-process counter must still separate them.
  CHECK(first.rfind("xenia_memory_", 0) == 0);
}

TEST_CASE("CreateFileMappingHandle never shares an existing name", "[memory][section]") {
  const std::string name = NextGuestSectionName() + "_shared_name";

  auto first = CreateTestSection(name);
  REQUIRE(first != rex::memory::kFileMappingHandleInvalid);
  auto* view = static_cast<uint8_t*>(rex::memory::MapFileView(
      first, nullptr, kTestSectionSize, rex::memory::PageAccess::kReadWrite, 0));
  REQUIRE(view != nullptr);
  std::memset(view, 0xA5, 64);

  // The old behavior opened the existing section here and aliased its memory.
  auto second = CreateTestSection(name);
  CHECK(second == rex::memory::kFileMappingHandleInvalid);
  if (second != rex::memory::kFileMappingHandleInvalid) {
    rex::memory::CloseFileMappingHandle(second, name);
  }

  rex::memory::UnmapFileView(first, view, kTestSectionSize);
  rex::memory::CloseFileMappingHandle(first, name);

  // Once the owner is gone the name is free again.
  auto reused = CreateTestSection(name);
  CHECK(reused != rex::memory::kFileMappingHandleInvalid);
  if (reused != rex::memory::kFileMappingHandleInvalid) {
    rex::memory::CloseFileMappingHandle(reused, name);
  }
}

TEST_CASE("Memory::Initialize skips a section name already in use", "[memory][section]") {
  rex::InitLogging();
  g_taken_name = NextGuestSectionName() + "_taken";
  g_free_name = NextGuestSectionName() + "_free";

  auto taken = CreateTestSection(g_taken_name);
  REQUIRE(taken != rex::memory::kFileMappingHandleInvalid);

  {
    ScopedNameSource source(&TakenThenFreeSource);
    rex::memory::Memory memory;
    REQUIRE(memory.Initialize());
    CHECK(memory.file_name().string() == g_free_name);
    CHECK(g_source_calls == 2);
  }

  rex::memory::CloseFileMappingHandle(taken, g_taken_name);
}

// Initialize reports this failure through assert_always, which aborts in debug builds.
#ifdef NDEBUG
TEST_CASE("Memory::Initialize gives up after a few taken names", "[memory][section]") {
  rex::InitLogging();
  g_taken_name = NextGuestSectionName() + "_always_taken";

  auto taken = CreateTestSection(g_taken_name);
  REQUIRE(taken != rex::memory::kFileMappingHandleInvalid);

  {
    ScopedNameSource source(&AlwaysTakenSource);
    rex::memory::Memory memory;
    CHECK_FALSE(memory.Initialize());
    CHECK(g_source_calls > 1);
    CHECK(g_source_calls < 16);
  }

  rex::memory::CloseFileMappingHandle(taken, g_taken_name);
}
#endif
