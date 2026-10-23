/**
 * @file        rex/ui/overlay/fps_counter_overlay.h
 *
 * @brief       Corner FPS counter fed by the guest frame statistics provider.
 */
#pragma once
#include <functional>

#include <rex/ui/frame_stats.h>
#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

class FpsCounterDialog : public ImGuiDialog {
 public:
  using FrameStatsProvider = std::function<FrameStats()>;

  explicit FpsCounterDialog(ImGuiDrawer* imgui_drawer, FrameStatsProvider stats_provider = {});
  ~FpsCounterDialog() override;

  void SetStatsProvider(FrameStatsProvider provider) { stats_provider_ = std::move(provider); }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  FrameStatsProvider stats_provider_;
};

}  // namespace rex::ui
