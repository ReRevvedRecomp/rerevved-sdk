/**
 * @file        audio/output_state.cpp
 * @brief       Output-stage policy: window focus and the player volume/silence rules
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <atomic>

#include <rex/audio/output_state.h>

namespace rex::audio {
namespace {

// Written on the UI thread, read in the SDL device callback. A lone flag
// needs no ordering with the other mix state, so relaxed is enough.
std::atomic<bool> g_focused{true};

}  // namespace

void SetOutputFocused(bool focused) {
  g_focused.store(focused, std::memory_order_relaxed);
}

bool IsOutputFocused() {
  return g_focused.load(std::memory_order_relaxed);
}

}  // namespace rex::audio
