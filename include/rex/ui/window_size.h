/**
 * @file        ui/window_size.h
 * @brief       Parsing and fitting for the `window_size` startup setting
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <string_view>

namespace rex::ui {

enum class WindowSizeKind {
  kUnset,      // Defer to `resolution` and the app default.
  kFixed,      // Start at width x height.
  kMaximized,  // Start maximized; the app default is the restore size.
};

struct WindowSize {
  WindowSizeKind kind = WindowSizeKind::kUnset;
  uint32_t width = 0;
  uint32_t height = 0;
};

inline constexpr uint32_t kMaxWindowSizeDimension = 8192;

/// Parses a `window_size` value: "maximized" or "<width>x<height>" with each
/// side in 1..8192. Case-insensitive. Empty and malformed values are kUnset.
inline WindowSize ParseWindowSize(std::string_view value) {
  constexpr std::string_view kMaximized = "maximized";
  if (value.size() == kMaximized.size()) {
    bool match = true;
    for (size_t i = 0; i < value.size(); ++i) {
      match = match && std::tolower(static_cast<unsigned char>(value[i])) == kMaximized[i];
    }
    if (match) {
      return {WindowSizeKind::kMaximized, 0, 0};
    }
  }
  const size_t x = value.find_first_of("xX");
  if (x == std::string_view::npos || x == 0 || x + 1 >= value.size()) {
    return {};
  }
  uint32_t parsed[2] = {};
  const std::string_view parts[2] = {value.substr(0, x), value.substr(x + 1)};
  for (int i = 0; i < 2; ++i) {
    const auto [end, error] =
        std::from_chars(parts[i].data(), parts[i].data() + parts[i].size(), parsed[i]);
    if (error != std::errc() || end != parts[i].data() + parts[i].size() || parsed[i] == 0 ||
        parsed[i] > kMaxWindowSizeDimension) {
      return {};
    }
  }
  return {WindowSizeKind::kFixed, parsed[0], parsed[1]};
}

/// The `window_size` that applies after `window_width`/`window_height`. An
/// explicit size (either > 0) outranks the setting entirely, including
/// "maximized", so such a request yields kUnset.
inline WindowSize EffectiveWindowSize(std::string_view value, int32_t window_width,
                                      int32_t window_height) {
  if (window_width > 0 || window_height > 0) {
    return {};
  }
  return ParseWindowSize(value);
}

/// Shrinks a size to fit within `max_width` x `max_height`, keeping the aspect
/// ratio (the result is never below 1x1). A size that already fits, or a
/// non-positive bound (unknown bounds), is returned unchanged.
inline WindowSize FitWindowSize(WindowSize size, int32_t max_width, int32_t max_height) {
  if (size.kind != WindowSizeKind::kFixed || max_width <= 0 || max_height <= 0) {
    return size;
  }
  const uint64_t mw = uint64_t(max_width);
  const uint64_t mh = uint64_t(max_height);
  if (size.width <= mw && size.height <= mh) {
    return size;
  }
  // Compare size.width / mw against size.height / mh exactly, then scale by
  // the tighter ratio.
  if (uint64_t(size.width) * mh >= uint64_t(size.height) * mw) {
    size.height = uint32_t(std::max<uint64_t>(1, uint64_t(size.height) * mw / size.width));
    size.width = uint32_t(mw);
  } else {
    size.width = uint32_t(std::max<uint64_t>(1, uint64_t(size.width) * mh / size.height));
    size.height = uint32_t(mh);
  }
  return size;
}

}  // namespace rex::ui
