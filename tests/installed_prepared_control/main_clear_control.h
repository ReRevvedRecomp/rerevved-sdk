#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <rex/graphics/draw_capture_mailbox.h>

namespace rex::graphics::testing {

struct MainClearCopyRecord {
  std::uint64_t id = 0;
  std::uint64_t frame = 0;
  std::uint64_t submission = 0;
  std::string mode;
  bool original_succeeded = false;
  std::uint32_t written_address = 0;
  std::uint32_t written_length = 0;
  std::uint32_t planned_address = 0;
  std::uint32_t planned_length = 0;
  bool cpu_data_complete = false;
  bool prepared = false;
};

struct MainClearControlResult {
  bool progressed = false;
  bool completion_observed = false;
  bool terminal_capture = false;
  std::uint32_t terminal_width = 0;
  std::uint32_t terminal_height = 0;
  std::uint32_t terminal_stride = 0;
  std::vector<std::uint8_t> terminal_pixels;
  std::vector<MainClearCopyRecord> original_copies;
  std::vector<MainClearCopyRecord> prepared_copies;
};

// Runs the installed PM4 control in this process. The optional observer is
// called synchronously for each borrowed Prepared/original view before the
// control releases the view, allowing a title harness to convert it directly.
int RunMainClearControl(
    const std::shared_ptr<rex::graphics::diagnostic::DrawCaptureFrameInputObserver>& observer,
    MainClearControlResult& result, std::string& error);

}  // namespace rex::graphics::testing
