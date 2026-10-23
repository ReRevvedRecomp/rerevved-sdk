/**
 * @file        rex/ui/overlay/asset_overlay_manager.h
 * @brief       Profile-local asset-overlay loadout manager
 */

#pragma once

#include <functional>
#include <string>
#include <vector>

#include <rex/system/asset_overlay_loadout.h>

namespace rex {
class Runtime;
}

namespace rex::ui {

struct Style;

class AssetOverlayManagerPane {
 public:
  AssetOverlayManagerPane(rex::Runtime* runtime, std::function<void()> close_callback = {});
  ~AssetOverlayManagerPane();

  bool RequestClose();
  void DrawContents(const Style& style);
  void DrawPopups(const Style& style);

 private:
  bool IsDirty() const;
  void ClearStatus();
  void DrawDiscardPopup(const Style& style);
  void DrawReplacementPopup(const Style& style);
  void ApplyStaged(bool replace_invalid_current);

  rex::Runtime* runtime_ = nullptr;
  std::function<void()> close_callback_;
  std::vector<std::string> staged_ids_;
  bool editing_priority_ = false;
  bool discard_popup_ = false;
  bool replacement_popup_ = false;
  std::string status_message_;
};

}  // namespace rex::ui
