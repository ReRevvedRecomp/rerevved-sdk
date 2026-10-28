/**
 * @file        ui/overlay/mod_manager_overlay.cpp
 * @brief       Profile-local mod loadout manager overlay
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/overlay/mod_manager_overlay.h>
#include <rex/ui/overlay/window_chrome.h>

#include <fstream>
#include <algorithm>
#include <vector>

#include <imgui.h>

#include <rex/runtime.h>
#include <rex/ui/image_decode.h>
#include <rex/ui/immediate_drawer.h>

namespace rex::ui {
namespace {

constexpr ImVec4 kHeaderText{0.60f, 0.85f, 1.00f, 1.00f};
constexpr ImVec4 kMutedText{0.60f, 0.62f, 0.66f, 1.00f};
constexpr ImVec4 kCodeBadge{1.00f, 0.82f, 0.30f, 1.00f};
constexpr float kIconSize = 40.0f;

const char* DiagnosticLabel(const rex::system::ModDiagnostic& diagnostic) {
  return diagnostic.severity == rex::system::ModDiagnosticSeverity::kError ? "error: "
                                                                           : "warning: ";
}

}  // namespace

ModManagerDialog::ModManagerDialog(ImGuiDrawer* imgui_drawer, ImmediateDrawer* immediate_drawer,
                                   rex::Runtime* runtime, std::function<void()> close_callback)
    : ImGuiDialog(imgui_drawer),
      immediate_drawer_(immediate_drawer),
      runtime_(runtime),
      close_callback_(std::move(close_callback)) {
  if (runtime_) {
    staged_ids_ = runtime_->mod_order_ids();
    asset_manager_ = std::make_unique<AssetOverlayManagerPane>(runtime_, [this]() {
      if (close_callback_) {
        close_callback_();
      }
    });
  }
}

ModManagerDialog::~ModManagerDialog() = default;

bool ModManagerDialog::IsDirty() const {
  return runtime_ && staged_ids_ != runtime_->mod_order_ids();
}

void ModManagerDialog::ClearStaleStatus() {
  status_message_.clear();
  replacement_popup_ = false;
}

bool ModManagerDialog::RequestClose() {
  if (IsDirty()) {
    discard_popup_ = true;
    return false;
  }
  return !asset_manager_ || asset_manager_->RequestClose();
}

void ModManagerDialog::ApplyStaged(bool replace_invalid_current) {
  if (!runtime_) {
    return;
  }
  const auto result = runtime_->ApplyModLoadout(staged_ids_, replace_invalid_current);
  if (result.status == rex::system::ModLoadoutApplyStatus::kInvalidCurrent) {
    replacement_popup_ = true;
    return;
  }
  if (result.succeeded()) {
    status_message_.clear();
    return;
  }
  status_message_.clear();
  if (!result.diagnostics.empty()) {
    status_message_ = result.diagnostics.front().message;
  } else {
    status_message_ = "Could not save the mod loadout.";
  }
}

void ModManagerDialog::DrawDiscardPopup() {
  bool request_asset_close = false;
  if (discard_popup_) {
    ImGui::OpenPopup("Discard unsaved changes?");
  }
  if (!ImGui::BeginPopupModal("Discard unsaved changes?", nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }
  ImGui::TextWrapped("Discard enabled and load-order changes?");
  if (OverlayButton(imgui_drawer()->style(), "Discard")) {
    staged_ids_ = runtime_ ? runtime_->mod_order_ids() : std::vector<std::string>{};
    discard_popup_ = false;
    ImGui::CloseCurrentPopup();
    request_asset_close = asset_manager_ != nullptr;
  }
  ImGui::SameLine();
  if (OverlayButton(imgui_drawer()->style(), "Keep editing")) {
    discard_popup_ = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
  if (request_asset_close && !asset_manager_->RequestClose()) {
    return;
  }
  if (request_asset_close || !asset_manager_) {
    if (close_callback_) {
      close_callback_();
    }
  }
}

void ModManagerDialog::DrawReplacementPopup() {
  if (replacement_popup_) {
    ImGui::OpenPopup("Replace invalid mod_order.txt?");
  }
  if (!ImGui::BeginPopupModal("Replace invalid mod_order.txt?", nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }
  ImGui::TextWrapped(
      "The current profile loadout has errors. Replace it with the selected packages?");
  if (OverlayButton(imgui_drawer()->style(), "Replace and save")) {
    replacement_popup_ = false;
    ImGui::CloseCurrentPopup();
    ApplyStaged(true);
  }
  ImGui::SameLine();
  if (OverlayButton(imgui_drawer()->style(), "Keep current file")) {
    replacement_popup_ = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

ImmediateTexture* ModManagerDialog::GetIcon(const rex::system::ModPackage& mod) {
  if (mod.icon_path.empty()) {
    return nullptr;
  }
  const std::string key = mod.icon_path.string();
  auto cached = icon_cache_.find(key);
  if (cached != icon_cache_.end()) {
    return cached->second.get();
  }

  std::unique_ptr<ImmediateTexture> texture;
  if (immediate_drawer_) {
    std::ifstream file(mod.icon_path, std::ios::binary | std::ios::ate);
    if (file) {
      std::streamsize length = file.tellg();
      if (length > 0) {
        file.seekg(0);
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        if (file.read(reinterpret_cast<char*>(bytes.data()), length)) {
          int width = 0;
          int height = 0;
          auto rgba = DecodeImageRGBA(bytes.data(), bytes.size(), width, height);
          if (!rgba.empty() && width > 0 && height > 0) {
            texture = immediate_drawer_->CreateTexture(
                static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                ImmediateTextureFilter::kLinear, false, rgba.data());
          }
        }
      }
    }
  }

  ImmediateTexture* result = texture.get();
  icon_cache_.emplace(key, std::move(texture));
  return result;
}

void ModManagerDialog::OnDraw(ImGuiIO& io) {
  const float scale = imgui_drawer()->overlay_scale();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 40.0f * scale), ImGuiCond_FirstUseEver,
                          ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(560.0f * scale, 480.0f * scale), ImGuiCond_FirstUseEver);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * scale, 12.0f * scale));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * scale, 6.0f * scale));

  bool open = true;
  if (BeginOverlayWindow(imgui_drawer()->style(), "Mods##overlay",
                         close_callback_ ? &open : nullptr, ImGuiWindowFlags_NoCollapse, 0.92f)) {
    if (close_callback_ && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      open = false;
    }
    if (ImGui::BeginTabBar("##package-tabs")) {
      if (ImGui::BeginTabItem("Mods")) {
        const auto* catalog = runtime_ ? &runtime_->mod_catalog() : nullptr;
        const size_t installed_count = catalog ? catalog->packages.size() : 0;
        const size_t staged_count = staged_ids_.size();
        std::optional<std::string> lock_reason;
        if (runtime_) {
          lock_reason = runtime_->mod_loadout_lock_reason();
        }
        const bool locked = lock_reason.has_value();

        if (editing_load_order_) {
          ImGui::PushStyleColor(ImGuiCol_Text, kHeaderText);
          ImGui::Text("Load order");
          ImGui::PopStyleColor();
          ImGui::TextColored(kMutedText, "Top = highest priority.");
        } else {
          ImGui::PushStyleColor(ImGuiCol_Text, kHeaderText);
          ImGui::Text("%zu installed | %zu enabled", installed_count, staged_count);
          ImGui::PopStyleColor();
          ImGui::TextColored(kMutedText, "Enable or disable mods, then save changes.");
          if (runtime_ && OverlayButton(imgui_drawer()->style(), "Rescan")) {
            runtime_->RescanModCatalog();
            ClearStaleStatus();
          }
          if (runtime_) {
            ImGui::SameLine();
            ImGui::BeginDisabled(locked);
            if (OverlayButton(imgui_drawer()->style(), "Edit Order")) {
              editing_load_order_ = true;
            }
            ImGui::EndDisabled();
          }
          ImGui::Separator();
        }
        if (locked) {
          ImGui::TextColored(kCodeBadge, "%s", lock_reason->c_str());
        }

        const auto staged = runtime_ ? runtime_->ValidateModLoadout(staged_ids_)
                                     : rex::system::ModLoadoutSelection{};
        const float footer_height =
            ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 16.0f;
        if (editing_load_order_) {
          ImGui::BeginChild("##modlist", ImVec2(0.0f, -footer_height), false);
          if (staged_ids_.empty()) {
            ImGui::TextDisabled("No enabled mods.");
          } else {
            ImGui::BeginDisabled(locked);
            for (size_t order = 0; order < staged_ids_.size(); ++order) {
              const std::string& id = staged_ids_[order];
              const auto* mod = catalog ? catalog->Find(id) : nullptr;
              const std::string display_name =
                  mod && !mod->display_name.empty() ? mod->display_name : id;
              ImGui::PushID(static_cast<int>(order));
              const float controls_width =
                  ImGui::GetFrameHeight() * 2.0f + ImGui::GetStyle().ItemSpacing.x;
              const ImVec2 label_pos = ImGui::GetCursorScreenPos();
              const float available_width = ImGui::GetContentRegionAvail().x;
              const float controls_x = ImGui::GetCursorPosX() + available_width - controls_width;
              ImGui::PushClipRect(label_pos,
                                  ImVec2(label_pos.x + available_width - controls_width -
                                             ImGui::GetStyle().ItemSpacing.x,
                                         label_pos.y + ImGui::GetFrameHeight()),
                                  true);
              if (mod) {
                ImGui::Text("%zu. %s", order + 1, display_name.c_str());
              } else {
                ImGui::TextColored(kCodeBadge, "%zu. %s (unavailable)", order + 1, id.c_str());
              }
              ImGui::PopClipRect();
              ImGui::SameLine(controls_x);
              if (order > 0) {
                if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
                  std::swap(staged_ids_[order], staged_ids_[order - 1]);
                  ClearStaleStatus();
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
                  ClearStaleStatus();
                }
              } else {
                ImGui::BeginDisabled();
                ImGui::ArrowButton("##down", ImGuiDir_Down);
                ImGui::EndDisabled();
              }
              ImGui::Separator();
              ImGui::PopID();
            }
            ImGui::EndDisabled();
          }
          ImGui::EndChild();
        } else {
          if (catalog) {
            bool has_nonblocking_catalog_diagnostics = false;
            for (const auto& diagnostic : catalog->diagnostics) {
              if (!diagnostic.blocking) {
                has_nonblocking_catalog_diagnostics = true;
                break;
              }
            }
            if (has_nonblocking_catalog_diagnostics) {
              for (const auto& diagnostic : catalog->diagnostics) {
                if (!diagnostic.blocking) {
                  ImGui::TextWrapped("%s%s", DiagnosticLabel(diagnostic),
                                     diagnostic.message.c_str());
                }
              }
              ImGui::Separator();
            }
          }

          if (runtime_ && !runtime_->mod_loadout_diagnostics().empty()) {
            ImGui::TextColored(kCodeBadge, "Current profile loadout has blocking errors:");
            for (const auto& diagnostic : runtime_->mod_loadout_diagnostics()) {
              if (diagnostic.line != 0) {
                ImGui::TextWrapped("- line %zu: %s", diagnostic.line, diagnostic.message.c_str());
              } else {
                ImGui::TextWrapped("- %s", diagnostic.message.c_str());
              }
            }
            ImGui::Separator();
          }

          if (!staged.IsValid()) {
            ImGui::TextColored(kCodeBadge, "Selected loadout has errors:");
            for (const auto& diagnostic : staged.diagnostics) {
              if (diagnostic.line != 0) {
                ImGui::TextWrapped("- line %zu: %s", diagnostic.line, diagnostic.message.c_str());
              } else {
                ImGui::TextWrapped("- %s", diagnostic.message.c_str());
              }
            }
            ImGui::Separator();
          }

          std::vector<std::string> unavailable_staged_ids;
          if (catalog) {
            for (const auto& id : staged_ids_) {
              if (!catalog->Find(id)) {
                unavailable_staged_ids.push_back(id);
              }
            }
          }
          const bool has_unavailable_active =
              runtime_ &&
              std::any_of(
                  runtime_->active_mod_states().begin(), runtime_->active_mod_states().end(),
                  [catalog](const auto& state) { return !catalog || !catalog->Find(state.id); });

          if ((!catalog || catalog->packages.empty()) && unavailable_staged_ids.empty() &&
              !has_unavailable_active) {
            ImGui::TextDisabled("No installed mod packages.");
          } else {
            ImGui::BeginChild("##modlist", ImVec2(0.0f, -footer_height), false);
            for (const auto& id : unavailable_staged_ids) {
              ImGui::PushID(("unavailable:" + id).c_str());
              ImGui::TextColored(kCodeBadge, "Unavailable enabled package: %s", id.c_str());
              ImGui::SameLine();
              ImGui::BeginDisabled(locked);
              if (OverlayButton(imgui_drawer()->style(), "remove", ImVec2(0, 0), true)) {
                staged_ids_.erase(std::remove(staged_ids_.begin(), staged_ids_.end(), id),
                                  staged_ids_.end());
                ClearStaleStatus();
              }
              ImGui::EndDisabled();
              ImGui::PopID();
            }
            if (runtime_) {
              for (const auto& active_mod : runtime_->active_mod_states()) {
                if (!catalog || !catalog->Find(active_mod.id)) {
                  ImGui::TextColored(kHeaderText, "Active package unavailable: %s",
                                     active_mod.id.c_str());
                }
              }
            }
            if (catalog) {
              std::vector<const rex::system::ModPackage*> sorted_packages;
              sorted_packages.reserve(catalog->packages.size());
              for (const auto& mod : catalog->packages) {
                sorted_packages.push_back(&mod);
              }
              std::sort(sorted_packages.begin(), sorted_packages.end(),
                        [](const auto* left, const auto* right) {
                          const std::string left_name =
                              left->display_name.empty()
                                  ? (left->id.empty() ? left->folder_name : left->id)
                                  : left->display_name;
                          const std::string right_name =
                              right->display_name.empty()
                                  ? (right->id.empty() ? right->folder_name : right->id)
                                  : right->display_name;
                          if (left_name != right_name) {
                            return left_name < right_name;
                          }
                          return left->folder_name < right->folder_name;
                        });

              for (const auto* mod : sorted_packages) {
                const std::string package_id = mod->id.empty() ? mod->folder_name : mod->id;
                const std::string package_key = mod->mod_root.generic_string();
                ImGui::PushID(package_key.c_str());
                bool is_staged = std::find(staged_ids_.begin(), staged_ids_.end(), package_id) !=
                                 staged_ids_.end();
                ImGui::BeginDisabled(locked);
                const bool toggled = ImGui::Checkbox("##enabled", &is_staged);
                ImGui::EndDisabled();
                if (toggled) {
                  ClearStaleStatus();
                  if (is_staged) {
                    staged_ids_.push_back(package_id);
                  } else {
                    staged_ids_.erase(
                        std::remove(staged_ids_.begin(), staged_ids_.end(), package_id),
                        staged_ids_.end());
                  }
                }
                ImGui::SameLine();
                if (ImmediateTexture* icon = GetIcon(*mod)) {
                  ImGui::ImageWithBg(reinterpret_cast<ImTextureID>(icon),
                                     ImVec2(kIconSize, kIconSize), ImVec2(0, 0), ImVec2(1, 1),
                                     ImVec4(0, 0, 0, 0), ImVec4(1, 1, 1, 1));
                } else {
                  ImGui::Dummy(ImVec2(kIconSize, kIconSize));
                }
                ImGui::SameLine();

                ImGui::BeginGroup();
                const std::string display_name =
                    mod->display_name.empty() ? package_id : mod->display_name;
                ImGui::Text("%s", display_name.c_str());
                if (!mod->version.empty()) {
                  ImGui::SameLine();
                  ImGui::TextColored(kMutedText, "v%s", mod->version.c_str());
                }
                if (!mod->author.empty()) {
                  ImGui::SameLine();
                  ImGui::TextColored(kMutedText, "Author: %s", mod->author.c_str());
                }
                if (mod->payload_kind == system::ModPayloadKind::kNative) {
                  ImGui::TextColored(kMutedText, "Native");
                }
                for (const auto& diagnostic : mod->diagnostics) {
                  ImGui::TextWrapped("%s%s", DiagnosticLabel(diagnostic),
                                     diagnostic.message.c_str());
                }
                if (!mod->description.empty()) {
                  ImGui::TextWrapped("%s", mod->description.c_str());
                }
                ImGui::EndGroup();
                ImGui::Separator();
                ImGui::PopID();
              }
            }
            ImGui::EndChild();
          }
        }
        ImGui::Separator();
        const bool has_persistable_change =
            runtime_ &&
            (IsDirty() || !runtime_->mod_order_file_exists() || runtime_->mod_order_file_invalid());
        const bool live_apply = runtime_ && runtime_->mod_script_host() &&
                                runtime_->mod_script_host()->supports_replacement();
        const bool can_save = (has_persistable_change || live_apply) && staged.IsValid() && !locked;
        if (editing_load_order_) {
          if (OverlayButton(imgui_drawer()->style(), "Done")) {
            editing_load_order_ = false;
          }
          ImGui::SameLine();
        }
        if (!can_save) {
          ImGui::BeginDisabled();
        }
        if (OverlayButton(imgui_drawer()->style(), "Save changes")) {
          if (runtime_->mod_order_file_invalid()) {
            replacement_popup_ = true;
          } else {
            ApplyStaged(false);
          }
        }
        if (!can_save) {
          ImGui::EndDisabled();
        }
        const bool selected_loadout_invalid = runtime_ && !staged.IsValid();
        const bool current_loadout_invalid =
            runtime_ && !runtime_->mod_loadout_diagnostics().empty();
        const bool has_error =
            !status_message_.empty() || selected_loadout_invalid || current_loadout_invalid;
        if (has_error) {
          ImGui::SameLine();
          if (!status_message_.empty()) {
            ImGui::TextColored(kCodeBadge, "%s", status_message_.c_str());
          } else if (selected_loadout_invalid) {
            ImGui::TextColored(kCodeBadge, "Fix errors before saving.");
          } else {
            ImGui::TextColored(kCodeBadge, "Save changes to replace the invalid loadout.");
          }
        } else if (IsDirty()) {
          ImGui::SameLine();
          ImGui::TextColored(kMutedText, "Unsaved changes");
        } else if (runtime_ && runtime_->restart_required()) {
          ImGui::SameLine();
          ImGui::TextColored(kCodeBadge, "Restart required for saved changes.");
        }
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Asset Overrides")) {
        if (asset_manager_) {
          asset_manager_->DrawContents(imgui_drawer()->style());
        }
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
  }
  ImGui::End();

  ImGui::PopStyleVar(2);
  if (!open && RequestClose()) {
    close_callback_();
  }
  DrawDiscardPopup();
  DrawReplacementPopup();
  if (asset_manager_) {
    asset_manager_->DrawPopups(imgui_drawer()->style());
  }
}

}  // namespace rex::ui
