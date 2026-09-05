/**
 * @file        ui/overlay/mod_manager_overlay.cpp
 * @brief       Read-only enabled-mod manager overlay
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/overlay/mod_manager_overlay.h>

#include <fstream>
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

std::string JoinPlatforms(const std::vector<std::string>& platforms) {
  std::string result;
  for (const auto& platform : platforms) {
    if (!result.empty()) {
      result += ", ";
    }
    result += platform;
  }
  return result;
}

}  // namespace

ModManagerDialog::ModManagerDialog(ImGuiDrawer* imgui_drawer, ImmediateDrawer* immediate_drawer,
                                   rex::Runtime* runtime)
    : ImGuiDialog(imgui_drawer), immediate_drawer_(immediate_drawer), runtime_(runtime) {}

ModManagerDialog::~ModManagerDialog() = default;

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
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 40.0f), ImGuiCond_FirstUseEver,
                          ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(560.0f, 480.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.92f);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));

  if (ImGui::Begin("Mods##overlay", nullptr, ImGuiWindowFlags_NoCollapse)) {
    const auto* catalog = runtime_ ? &runtime_->mod_catalog() : nullptr;
    const auto* selection_errors = runtime_ ? &runtime_->mod_selection_diagnostics() : nullptr;
    size_t count = catalog ? catalog->packages.size() : 0;

    ImGui::PushStyleColor(ImGuiCol_Text, kHeaderText);
    ImGui::Text("%zu mod package%s installed", count, count == 1 ? "" : "s");
    ImGui::PopStyleColor();
    ImGui::TextColored(kMutedText, "Catalog discovery is read-only; restart applies native code.");
    ImGui::Separator();
    ImGui::Spacing();

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
            ImGui::TextWrapped("%s%s", DiagnosticLabel(diagnostic), diagnostic.message.c_str());
          }
        }
        ImGui::Separator();
      }
    }
    if (selection_errors && !selection_errors->empty()) {
      ImGui::TextColored(kCodeBadge, "Enabled mod selection has blocking errors:");
      for (const auto& diagnostic : *selection_errors) {
        ImGui::TextWrapped("- %s", diagnostic.message.c_str());
      }
      ImGui::Separator();
    }

    if (!catalog || catalog->packages.empty()) {
      ImGui::TextDisabled("No installed mod packages.");
    } else {
      ImGui::BeginChild("##modlist", ImVec2(0.0f, 0.0f), false);
      for (const auto& mod : catalog->packages) {
        const std::string package_id = mod.id.empty() ? mod.folder_name : mod.id;
        const std::string package_key = mod.mod_root.generic_string();
        ImGui::PushID(package_key.c_str());
        if (ImmediateTexture* icon = GetIcon(mod)) {
          ImGui::ImageWithBg(reinterpret_cast<ImTextureID>(icon), ImVec2(kIconSize, kIconSize),
                             ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), ImVec4(1, 1, 1, 1));
        } else {
          ImGui::Dummy(ImVec2(kIconSize, kIconSize));
        }
        ImGui::SameLine();

        ImGui::BeginGroup();
        ImGui::Text("%s", mod.display_name.c_str());
        if (!mod.version.empty()) {
          ImGui::SameLine();
          ImGui::TextColored(kMutedText, "v%s", mod.version.c_str());
        }
        if (!mod.code.empty()) {
          ImGui::SameLine();
          ImGui::TextColored(kCodeBadge, "[code]");
        }
        ImGui::TextColored(kMutedText, "%s | %s", package_id.c_str(), mod.StatusText().c_str());
        ImGui::TextColored(kMutedText, "folder: %s", mod.folder_name.c_str());
        if (!mod.author.empty()) {
          ImGui::TextColored(kMutedText, "author: %s", mod.author.c_str());
        }
        if (!mod.available_platforms.empty()) {
          const auto platforms = JoinPlatforms(mod.available_platforms);
          ImGui::TextColored(kMutedText, "platform payloads: %s", platforms.c_str());
        }
        if (mod.desired) {
          ImGui::TextColored(kMutedText, "desired load order: #%zu",
                             mod.desired_order.value_or(0) + 1);
        }
        if (mod.active) {
          ImGui::TextColored(kHeaderText, "active load order: #%zu",
                             mod.active_order.value_or(0) + 1);
        }
        if (!mod.plugin_path.empty()) {
          ImGui::TextColored(kMutedText, "payload: %s", mod.plugin_path.string().c_str());
        }
        for (const auto& diagnostic : mod.diagnostics) {
          ImGui::TextWrapped("%s%s", DiagnosticLabel(diagnostic), diagnostic.message.c_str());
        }
        if (!mod.description.empty()) {
          ImGui::TextWrapped("%s", mod.description.c_str());
        }
        ImGui::EndGroup();
        ImGui::Separator();
        ImGui::PopID();
      }
      ImGui::EndChild();
    }
  }
  ImGui::End();

  ImGui::PopStyleVar(2);
}

}  // namespace rex::ui
