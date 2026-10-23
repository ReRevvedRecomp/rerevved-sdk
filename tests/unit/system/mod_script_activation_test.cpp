#include <catch2/catch_test_macros.hpp>
#include <rex/runtime.h>
#include <rex/system/mod_script.h>
#include <atomic>
#include <cstdlib>
#include <new>

// This executable compiles the production dispatch translation unit locally so
// its STL allocations use this allocator. Nothing is injected into the DLL ABI.
namespace {
thread_local bool count_allocations = false;
thread_local bool deny_allocations = false;
thread_local size_t allocation_count = 0;
thread_local size_t fail_at = 0;
struct AllocationScope {
  explicit AllocationScope(size_t fail = 0, bool deny = false) {
    allocation_count = 0;
    fail_at = fail;
    deny_allocations = deny;
    count_allocations = true;
  }
  ~AllocationScope() { count_allocations = deny_allocations = false; }
};
}  // namespace
void* operator new(std::size_t size) {
  if (count_allocations) {
    ++allocation_count;
    if (deny_allocations || (fail_at && allocation_count == fail_at))
      throw std::bad_alloc();
  }
  if (void* pointer = std::malloc(size ? size : 1))
    return pointer;
  throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept {
  std::free(pointer);
}
void* operator new[](std::size_t size) {
  return ::operator new(size);
}
void operator delete[](void* pointer) noexcept {
  std::free(pointer);
}

using namespace rex::system;
TEST_CASE("Lua activation preparation allocation failures leave no active state", "[mod_script]") {
  constexpr auto id = "a-long-package-identifier-requiring-owned-storage";
  size_t total = 0;
  {
    rex::Runtime runtime({}, {});
    {
      AllocationScope scope;
      auto activation = runtime.PrepareModScriptActivation(id, 0);
      total = allocation_count;
    }
    REQUIRE(runtime.active_mod_states().empty());
  }
  REQUIRE(total >= 2);
  for (size_t n = 1; n <= total; ++n) {
    rex::Runtime runtime({}, {});
    bool failed = false;
    {
      AllocationScope scope(n);
      try {
        auto activation = runtime.PrepareModScriptActivation(id, 0);
      } catch (const std::bad_alloc&) {
        failed = true;
      }
    }
    REQUIRE(failed);
    REQUIRE(runtime.active_mod_states().empty());
  }
}

TEST_CASE("Lua activation acknowledgement allocates nothing after publication", "[mod_script]") {
  rex::Runtime runtime({}, {});
  constexpr auto id = "a-long-package-identifier-requiring-owned-storage";
  auto activation = runtime.PrepareModScriptActivation(id, 3);
  size_t attempts = 0;
  {
    AllocationScope scope(0, true);
    activation.Commit();
    attempts = allocation_count;
  }
  REQUIRE(attempts == 0);
  REQUIRE(runtime.active_mod_states().size() == 1);
  REQUIRE(runtime.active_mod_states()[0].id == id);
  REQUIRE(runtime.active_mod_states()[0].order == 3);
}
