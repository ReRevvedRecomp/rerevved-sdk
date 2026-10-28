/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <ratio>
#include <type_traits>

#include <rex/chrono/chrono.h>

// This is in a separate header because casting to and from steady time points
// usually doesn't make sense and is imprecise. However, NT uses the FileTime
// epoch as a steady clock in waits. In such cases, include this header and use
// clock_cast<>().

namespace std::chrono {

// This conveniently works only for Host time domain because Guest needs
// additional scaling. Convert XSystemClock to WinSystemClock first if
// necessary.
template <>
struct clock_time_conversion<::rex::chrono::WinSystemClock, std::chrono::steady_clock> {
  // using NtSystemClock_ = ::rex::chrono::internal::NtSystemClock<domain_>;
  using WinSystemClock_ = ::rex::chrono::WinSystemClock;
  using steady_clock_ = std::chrono::steady_clock;

  template <typename Duration>
  typename WinSystemClock_::time_point operator()(
      const std::chrono::time_point<steady_clock_, Duration>& t) const {
    // Since there is no known epoch for steady_clock and even if, since it can
    // progress differently than other common clocks (e.g. stopping when the
    // computer is suspended), we need to use now() which introduces
    // imprecision.
    // Memory fences to keep the clock fetches close together to
    // minimize drift. This pattern was benchmarked to give the lowest
    // conversion error: error = sty_tpoint -
    // clock_cast<sty>(clock_cast<nt>(sty_tpoint));
    std::atomic_thread_fence(std::memory_order_acq_rel);
    auto steady_now = steady_clock_::now();
    auto nt_now = WinSystemClock_::now();
    std::atomic_thread_fence(std::memory_order_acq_rel);

    auto delta = std::chrono::floor<WinSystemClock_::duration>(t - steady_now);
    return nt_now + delta;
  }
};

template <>
struct clock_time_conversion<std::chrono::steady_clock, ::rex::chrono::WinSystemClock> {
  using WinSystemClock_ = ::rex::chrono::WinSystemClock;
  using steady_clock_ = std::chrono::steady_clock;

  template <typename Duration>
  steady_clock_::time_point operator()(
      const std::chrono::time_point<WinSystemClock_, Duration>& t) const {
    std::atomic_thread_fence(std::memory_order_acq_rel);
    auto steady_now = steady_clock_::now();
    auto nt_now = WinSystemClock_::now();
    std::atomic_thread_fence(std::memory_order_acq_rel);

    using SignedTicks = std::chrono::duration<int64_t, std::ratio<1, 10000000>>;
    using steady_duration = steady_clock_::duration;
    // steady_clock counts nanoseconds, so a delta past about 292 years overflows.
    // NT times near the FileTime epoch are legal already-due times, so inputs
    // outside the window saturate at its edge.
    constexpr auto kMaxDelta = std::chrono::duration_cast<SignedTicks>(std::chrono::years(10));
    constexpr auto kMaxSteady = std::chrono::duration_cast<steady_duration>(kMaxDelta);

    // Compare in the coarser of the input and 100 ns ticks so neither side
    // overflows when scaled. Flooring a finer input cannot cross a whole-tick edge.
    constexpr bool kInputIsCoarse =
        std::ratio_greater_equal_v<typename Duration::period, typename SignedTicks::period>;
    using Compare = std::conditional_t<kInputIsCoarse, Duration, SignedTicks>;
    const auto lower = std::chrono::duration_cast<Compare>((nt_now - kMaxDelta).time_since_epoch());
    const auto upper = std::chrono::duration_cast<Compare>((nt_now + kMaxDelta).time_since_epoch());
    const auto input = std::chrono::duration_cast<Compare>(t.time_since_epoch());
    if (input < lower) {
      return steady_now - kMaxSteady;
    }
    if (input > upper) {
      return steady_now + kMaxSteady;
    }

    // Inside the window everything is close to nt_now, so the 100 ns tick count
    // fits SignedTicks. Keep the sub-tick remainder of a finer input.
    const auto ticks = std::chrono::duration_cast<SignedTicks>(t.time_since_epoch());
    const auto remainder = t.time_since_epoch() - std::chrono::duration_cast<Duration>(ticks);
    return steady_now +
           std::chrono::duration_cast<steady_duration>(ticks - nt_now.time_since_epoch()) +
           std::chrono::duration_cast<steady_duration>(remainder);
  }
};

}  // namespace std::chrono
