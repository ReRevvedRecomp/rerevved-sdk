/**
 * Tests for ArgTranslator stack argument support and ImportFunction isolation.
 */

#include <cstdint>
#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include <rex/ppc/context.h>
#include <rex/ppc/function.h>

using namespace rex::ppc;

// Fake 64KB guest memory block for tests
alignas(64) static uint8_t g_test_mem[0x10000] = {};

TEST_CASE("SetIntegerArgumentValue writes stack args for index > 7", "[ppc][arg_translator]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;

  ArgTranslator::SetIntegerArgumentValue(ctx, g_test_mem, 8, 0xDEADBEEF);

  uint64_t readback = ArgTranslator::GetIntegerArgumentValue(ctx, g_test_mem, 8);
  CHECK(readback == 0xDEADBEEF);
}

TEST_CASE("SetIntegerArgumentValue writes multiple stack args at correct offsets",
          "[ppc][arg_translator]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;
  std::memset(g_test_mem + 0x8000, 0, 0x100);

  ArgTranslator::SetIntegerArgumentValue(ctx, g_test_mem, 8, 0x11111111);
  ArgTranslator::SetIntegerArgumentValue(ctx, g_test_mem, 9, 0x22222222);
  ArgTranslator::SetIntegerArgumentValue(ctx, g_test_mem, 10, 0x33333333);

  CHECK(ArgTranslator::GetIntegerArgumentValue(ctx, g_test_mem, 8) == 0x11111111);
  CHECK(ArgTranslator::GetIntegerArgumentValue(ctx, g_test_mem, 9) == 0x22222222);
  CHECK(ArgTranslator::GetIntegerArgumentValue(ctx, g_test_mem, 10) == 0x33333333);
}

TEST_CASE("SetIntegerArgumentValue still works for register args 0-7", "[ppc][arg_translator]") {
  PPCContext ctx{};
  ArgTranslator::SetIntegerArgumentValue(ctx, g_test_mem, 0, 0xAAAA);
  ArgTranslator::SetIntegerArgumentValue(ctx, g_test_mem, 7, 0xBBBB);
  CHECK(ctx.r3.u64 == 0xAAAA);
  CHECK(ctx.r10.u64 == 0xBBBB);
}

TEST_CASE("64-bit integer arguments read the whole register", "[ppc][arg_translator]") {
  PPCContext ctx{};
  ctx.r5.u64 = 0xB13EBABEBABEBABEull;

  CHECK(ArgTranslator::GetValue<uint64_t>(ctx, g_test_mem, 2) == 0xB13EBABEBABEBABEull);
  CHECK(ArgTranslator::GetValue<int64_t>(ctx, g_test_mem, 2) ==
        static_cast<int64_t>(0xB13EBABEBABEBABEull));
}

TEST_CASE("32-bit integer arguments keep the low word", "[ppc][arg_translator]") {
  PPCContext ctx{};
  // lis leaves sign-extension bits above a 32-bit guest value.
  ctx.r3.u64 = 0xFFFFFFFF82000000ull;

  CHECK(ArgTranslator::GetValue<uint32_t>(ctx, g_test_mem, 0) == 0x82000000u);
  CHECK(ArgTranslator::GetIntegerArgumentValue(ctx, g_test_mem, 0) == 0x82000000u);
}

TEST_CASE("64-bit stack arguments use the whole 8-byte slot", "[ppc][arg_translator]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;
  std::memset(g_test_mem + 0x8000, 0, 0x100);

  ArgTranslator::SetValue<uint64_t>(ctx, g_test_mem, 9, 0x0123456789ABCDEFull);

  CHECK(ArgTranslator::GetValue<uint64_t>(ctx, g_test_mem, 9) == 0x0123456789ABCDEFull);
  // The 32-bit view of the same slot is its low word.
  CHECK(ArgTranslator::GetValue<uint32_t>(ctx, g_test_mem, 9) == 0x89ABCDEFu);
  // Big-endian guest bytes at r1 + 0x58.
  CHECK(g_test_mem[0x8058] == 0x01);
  CHECK(g_test_mem[0x805F] == 0xEF);
}

TEST_CASE("64-bit register arguments round trip through SetValue", "[ppc][arg_translator]") {
  PPCContext ctx{};
  ArgTranslator::SetValue<uint64_t>(ctx, g_test_mem, 1, 0xFEDCBA9876543210ull);
  CHECK(ctx.r4.u64 == 0xFEDCBA9876543210ull);
}
