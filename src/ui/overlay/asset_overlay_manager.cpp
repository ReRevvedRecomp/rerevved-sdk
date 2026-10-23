

/**
 * @file        ui/overlay/asset_overlay_manager.cpp
 * @brief       Profile-local asset-overlay loadout manager
 */

#include <rex/ui/overlay/asset_overlay_manager.h>

#include <algorithm>

#include <imgui.h>

#include <rex/runtime.h>
#include <rex/ui/overlay/window_chrome.h>

namespace rex::ui {
namespace {

constexpr ImVec4 kHeaderText{0.60f, 0.85f, 1.00f, 1.00f};
constexpr ImVec4 kMutedText{0.60f, 0.62f, 0.66f, 1.00f};
constexpr ImVec4 kErrorText{1.00f, 0.82f, 0.30f, 1.00f};

}  // namespace

AssetOverlayManagerPane::AssetOverlayManagerPane(rex::Runtime* runtime,
                                                 std::function<void()> close_callback)
    : runtime_(runtime), close_callback_(std::move(close_callback)) {
  if (runtime_) {
    staged_ids_ = runtime_->asset_order_ids();
  }
}

AssetOverlayManagerPane::~AssetOverlayManagerPane() = default;

bool AssetOverlayManagerPane::IsDirty() const {
  return runtime_ && staged_ids_ != runtime_->asset_order_ids();
}

void AssetOverlayManagerPane::ClearStatus() {
  status_message_.clear();
  replacement_popup_ = false;
}

bool AssetOverlayManagerPane::RequestClose() {
  if (!IsDirty()) {
    return true;
  }
  discard_popup_ = true;
  return false;
}

void AssetOverlayManagerPane::ApplyStaged(bool replace_invalid_current) {
  if (!runtime_) {
    return;
  }
  const auto result = runtime_->ApplyAssetOverlayLoadout(staged_ids_, replace_invalid_current);
  if (result.status == rex::system::AssetOverlayLoadoutApplyStatus::kInvalidCurrent) {
    replacement_popup_ = true;
    return;
  }
  if (result.succeeded()) {
    status_message_.clear();
    return;
  }
  status_message_ = result.diagnostics.empty() ? "Could not save the asset overlay loadout."
                                               : result.diagnostics.front().message;
}

void AssetOverlayManagerPane::DrawDiscardPopup(const Style& style) {
  if (discard_popup_) {
    ImGui::OpenPopup("Discard asset overlay changes?");
  }
  if (!ImGui::BeginPopupModal("Discard asset overlay changes?", nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }
  ImGui::TextWrapped("Discard asset overlay enable and priority changes?");
  if (OverlayButton(style, "Discard")) {
    staged_ids_ = runtime_ ? runtime_->asset_order_ids() : std::vector<std::string>{};
    discard_popup_ = false;
    ImGui::CloseCurrentPopup();
    if (close_callback_) {
      close_callback_();
    }
  }
  ImGui::SameLine();
  if (OverlayButton(style, "Keep editing")) {
    discard_popup_ = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void AssetOverlayManagerPane::DrawReplacementPopup(const Style& style) {
  if (replacement_popup_) {
    ImGui::OpenPopup("Replace invalid asset_order.txt?");
  }
  if (!ImGui::BeginPopupModal("Replace invalid asset_order.txt?", nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }
  ImGui::TextWrapped(
      "The current asset overlay loadout has errors. Replace it with the selected packs?");
  if (OverlayButton(style, "Replace and save")) {
    replacement_popup_ = false;
    ImGui::CloseCurrentPopup();
    ApplyStaged(true);
  }
  ImGui::SameLine();
  if (OverlayButton(style, "Keep current file")) {
    replacement_popup_ = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void AssetOverlayManagerPane::DrawContents(const Style& style) {
  const auto* catalog = runtime_ ? &runtime_->asset_overlay_catalog() : nullptr;
  const auto staged = runtime_ ? runtime_->ValidateAssetOverlayLoadout(staged_ids_)
                               : rex::system::AssetOverlayLoadoutSelection{};
  const float footer_height =
      ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 16.0f;

  if (editing_priority_) {
    ImGui::PushStyleColor(ImGuiCol_Text, kHeaderText);
    ImGui::Text("Priority order");
    ImGui::PopStyleColor();
    ImGui::TextColored(kMutedText, "Top = highest priority.");
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, kHeaderText);
    ImGui::Text("%zu installed | %zu enabled", catalog ? catalog->packages.size() : 0,
                staged_ids_.size());
    ImGui::PopStyleColor();
    ImGui::TextColored(kMutedText, "Enable or disable asset overrides, then save changes.");
    if (runtime_ && OverlayButton(style, "Rescan")) {
      runtime_->RescanAssetOverlayCatalog();
      ClearStatus();
    }
    if (runtime_) {
      ImGui::SameLine();
      if (OverlayButton(style, "Edit Order")) {
        editing_priority_ = true;
      }
    }
    ImGui::Separator();
  }

  if (editing_priority_) {
    ImGui::BeginChild("##assetpriority", ImVec2(0.0f, -footer_height), false);
    if (staged_ids_.empty()) {
      ImGui::TextDisabled("No enabled asset overrides.");
    }
    for (size_t order = 0; order < staged_ids_.size(); ++order) {
      const auto& id = staged_ids_[order];
      const auto* package = catalog ? catalog->Find(id) : nullptr;
      ImGui::PushID(static_cast<int>(order));
      ImGui::Text("%zu. %s", order + 1, package ? package->display_name.c_str() : id.c_str());
      const float controls_width = ImGui::GetFrameHeight() * 2.0f + ImGui::GetStyle().ItemSpacing.x;
      const float available_width = ImGui::GetContentRegionAvail().x;
      const float controls_x = ImGui::GetCursorPosX() + available_width - controls_width;
      ImGui::SameLine(controls_x);
      if (order > 0) {
        if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
          std::swap(staged_ids_[order], staged_ids_[order - 1]);
          ClearStatus();
        }
      } else {
        ImGui::BeginDisabled();
        ImGui::ArrowButton("##up", ImGuiDir_Up);
        ImGui::EndDisabled();
      }
      ImGui::SameLine();
      if (order + 1 < staged_ids_.size()) {
        if (ImGui::ArrowButton("##down", ImGuiDir_Down)) {
          std::swap(staged_ids_[order], staged_ids_[order + 1]);
          ClearStatus();
        }
      } else {
        ImGui::BeginDisabled();
        ImGui::ArrowButton("##down", ImGuiDir_Down);
        ImGui::EndDisabled();
      }
      ImGui::Separator();
      ImGui::PopID();
    }
    ImGui::EndChild();
  } else {
    ImGui::BeginChild("##assetlist", ImVec2(0.0f, -footer_height), false);
    if (!staged.IsValid()) {
      ImGui::TextColored(kErrorText, "Selected loadout has errors:");
      for (const auto& diagnostic : staged.diagnostics) {
        ImGui::TextWrapped("- %s", diagnostic.message.c_str());
      }
      ImGui::Separator();
    }
    if (runtime_ && !runtime_->asset_loadout_diagnostics().empty()) {
      ImGui::TextColored(kErrorText, "Current asset_order.txt has errors:");
      for (const auto& diagnostic : runtime_->asset_loadout_diagnostics()) {
        ImGui::TextWrapped("- %s", diagnostic.message.c_str());
      }
      ImGui::Separator();
    }
    if (catalog) {
      std::vector<const rex::system::AssetOverlayPackage*> packages;
      for (const auto& package : catalog->packages) {
        packages.push_back(&package);
      }
      std::sort(packages.begin(), packages.end(), [](const auto* left, const auto* right) {
        if (left->display_name != right->display_name) {
          return left->display_name < right->display_name;
        }
        return left->folder_name < right->folder_name;
      });
      for (const auto* package : packages) {
        bool enabled =
            std::find(staged_ids_.begin(), staged_ids_.end(), package->id) != staged_ids_.end();
        ImGui::PushID(package->id.c_str());
        if (ImGui::Checkbox("##enabled", &enabled)) {
          if (enabled) {
            staged_ids_.push_back(package->id);
          } else {
            staged_ids_.erase(std::remove(staged_ids_.begin(), staged_ids_.end(), package->id),
                              staged_ids_.end());
          }
          ClearStatus();
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("%s", package->display_name.c_str());
        if (!package->version.empty()) {
          ImGui::SameLine();
          ImGui::TextColored(kMutedText, "v%s", package->version.c_str());
        }
        if (!package->author.empty()) {
          ImGui::SameLine();
          ImGui::TextColored(kMutedText, "Author: %s", package->author.c_str());
        }
        for (const auto& diagnostic : package->diagnostics) {
          ImGui::TextWrapped("%s", diagnostic.message.c_str());
        }
        if (!package->description.empty()) {
          ImGui::TextWrapped("%s", package->description.c_str());
        }
        ImGui::EndGroup();
        ImGui::Separator();
        ImGui::PopID();
      }
    }
    ImGui::EndChild();
  }

  ImGui::Separator();
  const bool can_save =
      runtime_ &&
      (IsDirty() || !runtime_->asset_order_file_exists() || runtime_->asset_order_file_invalid()) &&
      staged.IsValid();
  if (editing_priority_) {
    if (OverlayButton(style, "Done")) {
      editing_priority_ = false;
    }
    ImGui::SameLine();
  }
  if (!can_save) {
    ImGui::BeginDisabled();
  }
  if (OverlayButton(style, "Save changes")) {
    if (runtime_->asset_order_file_invalid()) {
      replacement_popup_ = true;
    } else {
      ApplyStaged(false);
    }
  }
  if (!can_save) {
    ImGui::EndDisabled();
  }
  if (!status_message_.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(kErrorText, "%s", status_message_.c_str());
  } else if (IsDirty()) {
    ImGui::SameLine();
    ImGui::TextColored(kMutedText, "Unsaved changes");
  } else if (runtime_ && runtime_->asset_overlay_restart_required()) {
    ImGui::SameLine();
    ImGui::TextColored(kErrorText, "Restart required for saved changes.");
  }
}

void AssetOverlayManagerPane::DrawPopups(const Style& style) {
  DrawDiscardPopup(style);
  DrawReplacementPopup(style);
}

}  // namespace rex::ui
