/**
 * @file        graphics/swap_presentation.h
 * @brief       Binds a swap's display aspect to the source content it presents
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

#include <rex/system/interfaces/graphics.h>

namespace rex::graphics {

// Identity of the texture contents a swap presents. The instance changes when
// the cache recreates the texture and the generation when watched guest memory
// changes under it, so an unchanged pair means unchanged pixels. The
// frontbuffer address alone cannot say this: the guest rewrites the same
// address for every new frame.
struct SwapContentKey {
  uint64_t instance = 0;
  uint64_t generation = 0;
  // Instances start at 1, so zero marks content whose identity could not be
  // established (for example one overwritten while it was being sampled).
  bool known() const { return instance != 0; }
  bool operator==(const SwapContentKey&) const = default;
};

// Chooses the display aspect of each swap so an aspect stays with its image: a
// swap that repeats the previous content keeps that content's aspect whatever
// the new request says, and new content takes the request or, without one, the
// native aspect. Commit applies a decision once the presenter accepted the image,
// so a failed refresh leaves the shown image's record intact.
class SwapAspectTracker {
 public:
  struct Decision {
    uint32_t display_x = 0;
    uint32_t display_y = 0;
    bool content_known = false;
    bool repeated = false;
    uint64_t content_id = 0;
    SwapContentKey key;
  };

  // `content` is null when the backend could not identify the source.
  Decision Decide(const system::SwapPresentationAspect& request, const SwapContentKey* content,
                  uint32_t native_x, uint32_t native_y) const {
    Decision decision;
    decision.content_known = content != nullptr;
    if (content) {
      decision.key = *content;
      if (has_last_ && last_key_ == *content) {
        decision.repeated = true;
        decision.content_id = last_id_;
        decision.display_x = last_x_;
        decision.display_y = last_y_;
        return decision;
      }
      decision.content_id = next_id_;
    }
    const bool use_override = request.override_valid && request.aspect_x && request.aspect_y;
    decision.display_x = use_override ? request.aspect_x : native_x;
    decision.display_y = use_override ? request.aspect_y : native_y;
    return decision;
  }

  void Commit(const Decision& decision) {
    if (!decision.content_known) {
      has_last_ = false;
      return;
    }
    if (!decision.repeated) {
      ++next_id_;
    }
    has_last_ = true;
    last_key_ = decision.key;
    last_id_ = decision.content_id;
    last_x_ = decision.display_x;
    last_y_ = decision.display_y;
  }

  void Reset() { has_last_ = false; }

 private:
  bool has_last_ = false;
  SwapContentKey last_key_;
  uint64_t last_id_ = 0;
  uint32_t last_x_ = 0;
  uint32_t last_y_ = 0;
  uint64_t next_id_ = 1;
};

}  // namespace rex::graphics
