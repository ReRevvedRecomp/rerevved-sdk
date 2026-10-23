/**
 * @file        rex/ui/overlay/settings_overlay.h
 *
 * @brief       ImGui settings overlay dialog for cvar editing with save and revert.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/overlay/settings_layout.h>

namespace rex::ui {

class SettingsDialog : public ImGuiDialog {
 public:
  // config_path: where Save writes (e.g. exe_dir / "app.toml").
  // close_callback: asked to destroy the dialog after the close button or
  // Escape; without it the dialog has no close control.
  // layout: tabs, tiers and labels; empty keeps the category tree.
  SettingsDialog(ImGuiDrawer* imgui_drawer, std::filesystem::path config_path,
                 std::function<void()> close_callback = {}, SettingsLayout layout = {});
  ~SettingsDialog();

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  // Records the values Save would write, as the baseline for Revert.
  void SnapshotSavedValues();

  std::filesystem::path config_path_;
  std::function<void()> close_callback_;
  SettingsLayout layout_;
  char search_buf_[128] = {};
  std::string selected_category_;  // category path, or tab title with a layout
  std::string capturing_bind_name_;
  std::unordered_map<std::string, std::string> saved_values_;
  bool save_failed_ = false;
};

}  // namespace rex::ui
