/**
 * @file        rex/ui/style.h
 *
 * @brief       Per-overlay styling that ImGuiStyle cannot express.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     ImGuiDrawer owns one Style; overlays read their own section
 *              through ImGuiDialog::imgui_drawer()->style(). Consumers patch
 *              it from ReXApp::OnConfigureStyle. Every default matches the
 *              value used by the corresponding overlay; chrome and scale
 *              default to off.
 */
#pragma once
#include <functional>

#include <imgui.h>

namespace rex::ui {

struct AchievementsStyle {
  ImVec4 unlocked_title{0.45f, 1.00f, 0.55f, 1.00f};
  ImVec4 unlocked_desc{0.70f, 0.85f, 0.72f, 1.00f};
  ImVec4 locked_title{0.78f, 0.80f, 0.84f, 1.00f};
  ImVec4 locked_desc{0.50f, 0.52f, 0.56f, 1.00f};
  ImVec4 badge_gamerscore{1.00f, 0.82f, 0.30f, 1.00f};
  ImVec4 row_unlocked_bg{0.16f, 0.30f, 0.18f, 0.55f};
  ImVec4 header_text{0.60f, 0.85f, 1.00f, 1.00f};
  ImVec4 progress_bar{0.30f, 0.80f, 0.40f, 1.00f};
  ImVec4 unlocked_icon_tint{1.00f, 1.00f, 1.00f, 1.00f};
  ImVec4 locked_icon_tint{0.45f, 0.45f, 0.45f, 0.80f};
  ImVec2 window_padding{14.0f, 12.0f};
  ImVec2 item_spacing{8.0f, 6.0f};
  float icon_size = 44.0f;
  float row_rounding = 3.0f;
};

struct ToastStyle {
  /// Alpha is the fade envelope, applied on top of these at draw time.
  ImVec4 text{1.00f, 1.00f, 1.00f, 1.00f};
  ImVec4 title{1.00f, 0.85f, 0.20f, 1.00f};
  float rounding = 6.0f;
  float background_alpha = 0.88f;
  float icon_size = 44.0f;
  /// Fraction of the display height kept clear at the bottom, such as for a
  /// title's HUD; both toasts sit above it.
  float bottom_inset = 0.0f;
};

struct ConsoleStyle {
  /// Indexed by spdlog::level::level_enum: trace, debug, info, warn, err,
  /// critical, off.
  ImVec4 level[7] = {
      {0.5f, 0.5f, 0.5f, 1.0f},  // trace
      {0.4f, 0.9f, 0.9f, 1.0f},  // debug
      {1.0f, 1.0f, 1.0f, 1.0f},  // info
      {1.0f, 1.0f, 0.0f, 1.0f},  // warn
      {1.0f, 0.4f, 0.4f, 1.0f},  // err
      {1.0f, 0.0f, 0.0f, 1.0f},  // critical
      {1.0f, 1.0f, 1.0f, 1.0f},  // off
  };
  ImVec4 completion_bg{0.10f, 0.10f, 0.10f, 0.95f};
  ImVec4 completion_highlight{0.235f, 0.353f, 0.549f, 0.784f};
};

struct DebugOverlayStyle {
  ImVec4 muted_text{1.00f, 1.00f, 1.00f, 0.50f};
};

struct SettingsStyle {
  ImVec4 warning{1.00f, 0.80f, 0.20f, 1.00f};
  /// Cvar lifecycle badges: [live], [restart], [init-only].
  ImVec4 lifecycle_live{0.40f, 1.00f, 0.40f, 1.00f};
  ImVec4 lifecycle_restart{1.00f, 1.00f, 0.40f, 1.00f};
  ImVec4 lifecycle_init_only{1.00f, 0.40f, 0.40f, 1.00f};
  ImVec4 lifecycle_unknown{1.00f, 1.00f, 1.00f, 1.00f};
};

/// Interaction state passed to the button and selection skins.
enum class ChromeState { kNormal, kHovered, kActive, kSelected };

/// Optional skin for top-level overlay windows, their inner panels, buttons
/// and list selections. Empty callbacks keep the plain ImGui look.
struct ChromeStyle {
  /// Draws the window frame and background over [min, max]. title_height is
  /// zero for untitled windows.
  std::function<void(ImDrawList* draw_list, ImVec2 min, ImVec2 max, float title_height)>
      draw_window;
  /// Draws an inner panel, such as a list or the FPS counter background.
  std::function<void(ImDrawList* draw_list, ImVec2 min, ImVec2 max)> draw_panel;
  /// Draws a button background under its label, through OverlayButton. A
  /// disabled button arrives with ImGui::GetStyle().Alpha already reduced.
  std::function<void(ImDrawList* draw_list, ImVec2 min, ImVec2 max, ChromeState state)> draw_button;
  /// Draws a hovered, pressed or selected list row under its label, through
  /// OverlaySelectable. Never called with kNormal.
  std::function<void(ImDrawList* draw_list, ImVec2 min, ImVec2 max, ChromeState state)>
      draw_selection;
  /// Title band height in multiples of the current font size.
  float title_height_em = 1.8f;
  /// Extra title text indent past the window padding, in multiples of the
  /// current font size, for frames with wide corners.
  float title_indent_em = 0.0f;
};

/// Scales overlay fonts and sizes with the output height.
struct OverlayScaleStyle {
  /// Output height drawn at 1x. Zero disables scaling.
  float reference_height = 0.0f;
  float min_scale = 1.0f;
  float max_scale = 4.0f;
};

struct FpsCounterStyle {
  ImVec4 text{1.00f, 1.00f, 1.00f, 1.00f};
  float background_alpha = 0.55f;
  /// Distance from the top-right corner, before overlay scale.
  ImVec2 margin{12.0f, 12.0f};
};

struct Style {
  AchievementsStyle achievements;
  ToastStyle toast;
  ConsoleStyle console;
  DebugOverlayStyle debug;
  SettingsStyle settings;
  ChromeStyle chrome;
  OverlayScaleStyle scale;
  FpsCounterStyle fps_counter;
};

}  // namespace rex::ui
