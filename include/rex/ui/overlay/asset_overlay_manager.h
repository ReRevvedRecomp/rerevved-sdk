/**
 * @file        rex/ui/overlay/asset_overlay_manager.h
 * @brief       Profile-local asset-overlay loadout manager
 */

#pragma once

#include <functional>
#include <span>
#include <string>
#include <vector>

#include <rex/system/asset_overlay_loadout.h>

namespace rex {
class Runtime;
}

namespace rex::ui {

struct Style;

// Apps add tabs via ReXApp::OnConfigureModManager; every function is required.
struct AssetOverlayLoadoutSource {
  std::string title;
  std::string noun;  // plural for sentences, e.g. "map packs"
  std::string kind;  // singular for sentences, e.g. "map pack"
  std::string order_file;
  std::function<const rex::system::AssetOverlayCatalog&()> catalog;
  std::function<std::vector<std::string>()> order_ids;
  std::function<rex::system::AssetOverlayLoadoutSelection(std::span<const std::string>)> validate;
  std::function<rex::system::AssetOverlayLoadoutApplyResult(std::span<const std::string>, bool)>
      apply;
  std::function<void()> rescan;
  std::function<std::vector<rex::system::AssetOverlayLoadoutDiagnostic>()> diagnostics;
  std::function<bool()> file_exists;
  std::function<bool()> file_invalid;
  std::function<bool()> restart_required;
};

class AssetOverlayManagerPane {
 public:
  AssetOverlayManagerPane(rex::Runtime* runtime, std::function<void()> close_callback = {});
  AssetOverlayManagerPane(AssetOverlayLoadoutSource source,
                          std::function<void()> close_callback = {});
  ~AssetOverlayManagerPane();

  bool RequestClose();
  const std::string& title() const { return source_.title; }
  void DrawContents(const Style& style);
  void DrawPopups(const Style& style);

 private:
  bool IsDirty() const;
  // Rescans and saves update the staged order only if the user has not edited it.
  void SyncSaved();
  void ClearStatus();
  void DrawDiscardPopup(const Style& style);
  void DrawReplacementPopup(const Style& style);
  void ApplyStaged(bool replace_invalid_current);

  AssetOverlayLoadoutSource source_;
  std::string discard_title_;
  std::string replace_title_;
  std::function<void()> close_callback_;
  std::vector<std::string> staged_ids_;
  std::vector<std::string> saved_ids_;
  bool editing_priority_ = false;
  bool discard_popup_ = false;
  bool replacement_popup_ = false;
  std::string status_message_;
};

}  // namespace rex::ui
