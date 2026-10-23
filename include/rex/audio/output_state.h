/**
 * @file        audio/output_state.h
 * @brief       Output-stage policy: window focus and the player volume/silence rules
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <algorithm>
#include <cstdint>

namespace rex::audio {

/// Publishes whether the application's main window has focus and is not
/// minimized. ReXApp calls this from the UI thread; the output stage reads it
/// from the device callback. Safe to call from any thread. Defaults to true,
/// so a title that never reports focus is never silenced by
/// `audio_play_in_background`.
void SetOutputFocused(bool focused);
bool IsOutputFocused();

/// The gain the output stage applies: the master gain set through
/// SetOutputGain (titles, mods, plugins) times the player's `audio_volume`
/// percent. The percent is clamped to 0..100, so the player setting can only
/// attenuate what the master gain allows.
inline float ComposeOutputGain(float master_gain, int32_t volume_percent) {
  return master_gain * (float(std::clamp<int32_t>(volume_percent, 0, 100)) / 100.0f);
}

/// True when the output stage writes silence instead of the mix: the player
/// muted, or turned off background play while the window is not focused.
inline bool ShouldSilenceOutput(bool muted, bool play_in_background, bool window_focused) {
  return muted || (!play_in_background && !window_focused);
}

}  // namespace rex::audio
