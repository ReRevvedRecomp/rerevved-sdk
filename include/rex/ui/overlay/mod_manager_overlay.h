/**
 * @file        rex/ui/overlay/mod_manager_overlay.h
 * @brief       Mod selection and load order for the active profile
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/system/mod_catalog.h>
#include <rex/system/mod_loadout.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/overlay/asset_overlay_manager.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::ui {

class ImmediateDrawer;
class ImmediateTexture;

class ModManagerDialog : public ImGuiDialog {
 public:
  ModManagerDialog(ImGuiDrawer* imgui_drawer, ImmediateDrawer* immediate_drawer,
                   rex::Runtime* runtime, std::function<void()> close_callback = {});
  ~ModManagerDialog() override;

  // Returns true when the manager can close immediately. If staged edits are
  // present, the dialog opens a discard confirmation and returns false.
  bool RequestClose();

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  ImmediateTexture* GetIcon(const rex::system::ModPackage& mod);
  bool IsDirty() const;
  void ClearStaleStatus();
  void DrawDiscardPopup();
  void DrawReplacementPopup();
  void ApplyStaged(bool replace_invalid_current);

  ImmediateDrawer* immediate_drawer_ = nullptr;
  rex::Runtime* runtime_ = nullptr;
  std::function<void()> close_callback_;
  std::unique_ptr<AssetOverlayManagerPane> asset_manager_;
  std::unordered_map<std::string, std::unique_ptr<ImmediateTexture>> icon_cache_;
  std::vector<std::string> staged_ids_;
  bool editing_load_order_ = false;
  bool discard_popup_ = false;
  bool replacement_popup_ = false;
  std::string status_message_;
};

}  // namespace rex::ui
