#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <rex/graphics/draw_capture_mailbox.h>
#include <rex/graphics/shared_memory.h>

namespace rex::graphics::testing {

class TestSharedMemory final : public SharedMemory {
 public:
  explicit TestSharedMemory(rex::memory::Memory& memory) : SharedMemory(memory) {
    InitializeCommon();
  }

 private:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>&) override { return false; }
};

// Test adapter owns every BuildPrepared borrowed span and keeps analyzed guest
// shader identity, current register bank, and guard callbacks alive until the
// synchronous title collector copies the view.
class PreparedFrameInputTestSupport {
 public:
  PreparedFrameInputTestSupport();
  // Reuse the unit binary's initialized Memory singleton.
  explicit PreparedFrameInputTestSupport(rex::memory::Memory& memory);
  ~PreparedFrameInputTestSupport();
  PreparedFrameInputTestSupport(const PreparedFrameInputTestSupport&) = delete;
  PreparedFrameInputTestSupport& operator=(const PreparedFrameInputTestSupport&) = delete;

  // Build a prepared draw from the analyzed SDK shader objects. Native source
  // state is armed separately at the actual producer copy boundary.
  bool BuildConsumerDraw(std::string& error, bool native_source_ready = false);
  bool BuildClearDraw(std::string& error);
  bool BuildResolveCopy(std::uint64_t id, std::uint64_t frame, std::uint64_t submission,
                        std::uint32_t destination, bool small_source,
                        diagnostic::DrawCaptureFrameInputCopyView& copy, std::string& error);
  bool ArmNativeSource(std::string& error, std::uint64_t copy_id = 10);
  bool CompleteNativeSource(bool original_succeeded, std::string& error);
  // Mutates the clear producer's guest alpha before a fresh clear admission.
  // The native consumer shader samples this channel, making producer changes
  // observable without introducing a CPU texture owner.
  void SetClearAlpha(float alpha);

  const diagnostic::DrawCaptureFrameInputDrawView& draw() const;
  const diagnostic::DrawCaptureFrameInputDrawView& clear_draw() const;
  std::span<const std::uint32_t> registers() const;
  std::span<const std::uint32_t> vertex_words() const;
  std::span<const std::uint32_t> pixel_words() const;

  std::uint64_t vertex_shader_hash() const;
  std::uint64_t pixel_shader_hash() const;
  const std::string& vertex_word_digest() const;
  const std::string& pixel_word_digest() const;
  std::uint32_t texture_fetch_constant() const;
  std::size_t sampler_calls() const;
  std::size_t readable_calls() const;
  std::size_t version_calls() const;
  std::uint32_t texture_base() const;
  std::uint32_t texture_base_size() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rex::graphics::testing
