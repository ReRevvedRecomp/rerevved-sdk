
/**
 * @file        ui/overlay/fps_counter_overlay.cpp
 *
 * @brief       Corner FPS counter. See fps_counter_overlay.h for details.
 */

#include <rex/ui/overlay/fps_counter_overlay.h>

#include <cmath>
#include <cstdio>

#include <imgui.h>

namespace rex::ui {

FpsCounterDialog::FpsCounterDialog(ImGuiDrawer* imgui_drawer, FrameStatsProvider stats_provider)
    : ImGuiDialog(imgui_drawer), stats_provider_(std::move(stats_provider)) {}

FpsCounterDialog::~FpsCounterDialog() = default;

void FpsCounterDialog::OnDraw(ImGuiIO& io) {
  const Style& style = imgui_drawer()->style();
  const FpsCounterStyle& counter = style.fps_counter;
  const float scale = imgui_drawer()->overlay_scale();

  char text[32];
  const FrameStats stats = stats_provider_ ? stats_provider_() : FrameStats{};
  if (stats.frame_count > 0) {
    std::snprintf(text, sizeof(text), "%.0f FPS", std::round(stats.fps));
  } else {
    std::snprintf(text, sizeof(text), "-- FPS");
  }

  ImGui::SetNextWindowPos(
      ImVec2(io.DisplaySize.x - counter.margin.x * scale, counter.margin.y * scale),
      ImGuiCond_Always, ImVec2(1.0f, 0.0f));
  ImGui::SetNextWindowBgAlpha(style.chrome.draw_panel ? 0.0f : counter.background_alpha);
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (ImGui::Begin("##rex_fps_counter", nullptr, flags)) {
    if (style.chrome.draw_panel) {
      const ImVec2 min = ImGui::GetWindowPos();
      const ImVec2 size = ImGui::GetWindowSize();
      ImDrawList* draw_list = ImGui::GetWindowDrawList();
      draw_list->PushClipRect(min, ImVec2(min.x + size.x, min.y + size.y), false);
      style.chrome.draw_panel(draw_list, min, ImVec2(min.x + size.x, min.y + size.y));
      draw_list->PopClipRect();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, counter.text);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
  }
  ImGui::End();
}

}  // namespace rex::ui
