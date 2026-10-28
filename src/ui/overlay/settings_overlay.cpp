/**
 * @file        ui/overlay/settings_overlay.cpp
 *
 * @brief       Settings overlay implementation. See settings_overlay.h for details.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/settings_overlay.h>
#include <rex/cvar.h>
#include <rex/string.h>
#include <rex/string/numeric.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/monitors.h>
#include <rex/ui/overlay/window_chrome.h>
#include <imgui.h>

#include "settings_rows.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <string>
#include <vector>

REXCVAR_DEFINE_BOOL(settings_show_advanced, false, "UI/Settings",
                    "Show advanced settings in the settings overlay")
    .audience(rex::cvar::Audience::kHidden);

REXCVAR_DEFINE_BOOL(settings_show_developer, false, "UI/Settings",
                    "Show developer settings in the settings overlay")
    .audience(rex::cvar::Audience::kHidden);

namespace rex::ui {

template <typename T>
static void InputInteger(const rex::cvar::FlagEntry& entry, const std::string& text,
                         ImGuiDataType type) {
  T value{};
  std::from_chars(text.data(), text.data() + text.size(), value);
  const T step = 1, fast_step = 100;
  if (!ImGui::InputScalar("##v", type, &value, &step, &fast_step))
    return;
  auto bound = [](double limit) -> T {
    // Comparing before conversion also handles rounded double representations
    // of INT64_MAX and UINT64_MAX without an out-of-range integer cast.
    if (limit <= static_cast<double>(std::numeric_limits<T>::lowest()))
      return std::numeric_limits<T>::lowest();
    if (limit >= static_cast<double>(std::numeric_limits<T>::max()))
      return std::numeric_limits<T>::max();
    return static_cast<T>(limit);
  };
  if (entry.constraints.min)
    value = std::max(value, bound(std::ceil(*entry.constraints.min)));
  if (entry.constraints.max)
    value = std::min(value, bound(std::floor(*entry.constraints.max)));
  rex::cvar::SetFlagByName(entry.name, std::to_string(value));
}

SettingsDialog::SettingsDialog(ImGuiDrawer* imgui_drawer, std::filesystem::path config_path,
                               std::function<void()> close_callback, SettingsLayout layout)
    : ImGuiDialog(imgui_drawer),
      config_path_(std::move(config_path)),
      close_callback_(std::move(close_callback)),
      layout_(std::move(layout)) {
  if (!layout_.choice_labels.contains("monitor")) {
    layout_.choice_labels["monitor"] = MonitorChoiceLabels();
  }
  SnapshotSavedValues();
}

SettingsDialog::~SettingsDialog() {}

void SettingsDialog::SnapshotSavedValues() {
  saved_values_.clear();
  for (auto& entry : rex::cvar::GetRegistry()) {
    if (settings_rows::IsSaved(entry)) {
      saved_values_[entry.name] = entry.getter();
    }
  }
}

static ImVec4 LifecycleColor(rex::cvar::Lifecycle lc, const SettingsStyle& style) {
  switch (lc) {
    case rex::cvar::Lifecycle::kHotReload:
      return style.lifecycle_live;
    case rex::cvar::Lifecycle::kRequiresRestart:
      return style.lifecycle_restart;
    case rex::cvar::Lifecycle::kInitOnly:
      return style.lifecycle_init_only;
  }
  return style.lifecycle_unknown;
}

static bool IsKeybindCategory(std::string_view cat) {
  return cat == "Input/Keybinds" || cat.starts_with("Input/Keybinds/");
}

// Custom text or default details, with lifecycle coloring; see PlanRowTooltip.
static void RowTooltip(const rex::cvar::FlagEntry& entry, const SettingsStyle& style,
                       const settings_rows::RowTooltipPlan& plan) {
  if (!plan.visible || !ImGui::BeginTooltip()) {
    return;
  }
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
  if (plan.text) {
    ImGui::TextUnformatted(plan.text->data(), plan.text->data() + plan.text->size());
  }
  if (plan.details) {
    if (!entry.description.empty()) {
      ImGui::TextUnformatted(entry.description.c_str());
    }
    ImGui::TextDisabled("%s", entry.name.c_str());
    ImGui::SameLine();
  }
  if (plan.lifecycle) {
    ImGui::TextColored(LifecycleColor(entry.lifecycle, style), "%s",
                       settings_rows::LifecycleText(entry.lifecycle));
  }
  ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
}

static rex::ui::VirtualKey ImGuiKeyToVirtualKey(ImGuiKey key) {
  using VK = rex::ui::VirtualKey;
  if (key >= ImGuiKey_A && key <= ImGuiKey_Z) {
    return static_cast<VK>(static_cast<uint16_t>(VK::kA) + (key - ImGuiKey_A));
  }
  if (key >= ImGuiKey_0 && key <= ImGuiKey_9) {
    return static_cast<VK>(static_cast<uint16_t>(VK::k0) + (key - ImGuiKey_0));
  }
  if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24) {
    return static_cast<VK>(static_cast<uint16_t>(VK::kF1) + (key - ImGuiKey_F1));
  }
  if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9) {
    return static_cast<VK>(static_cast<uint16_t>(VK::kNumpad0) + (key - ImGuiKey_Keypad0));
  }
  switch (key) {
    case ImGuiKey_Space:
      return VK::kSpace;
    case ImGuiKey_Enter:
      return VK::kReturn;
    case ImGuiKey_Escape:
      return VK::kEscape;
    case ImGuiKey_Tab:
      return VK::kTab;
    case ImGuiKey_Backspace:
      return VK::kBack;
    case ImGuiKey_Delete:
      return VK::kDelete;
    case ImGuiKey_Insert:
      return VK::kInsert;
    case ImGuiKey_Home:
      return VK::kHome;
    case ImGuiKey_End:
      return VK::kEnd;
    case ImGuiKey_PageUp:
      return VK::kPrior;
    case ImGuiKey_PageDown:
      return VK::kNext;
    case ImGuiKey_LeftArrow:
      return VK::kLeft;
    case ImGuiKey_RightArrow:
      return VK::kRight;
    case ImGuiKey_UpArrow:
      return VK::kUp;
    case ImGuiKey_DownArrow:
      return VK::kDown;
    case ImGuiKey_LeftShift:
    case ImGuiKey_RightShift:
      return VK::kShift;
    case ImGuiKey_LeftCtrl:
    case ImGuiKey_RightCtrl:
      return VK::kControl;
    case ImGuiKey_LeftAlt:
    case ImGuiKey_RightAlt:
      return VK::kMenu;
    case ImGuiKey_CapsLock:
      return VK::kCapital;
    case ImGuiKey_NumLock:
      return VK::kNumLock;
    case ImGuiKey_ScrollLock:
      return VK::kScroll;
    case ImGuiKey_PrintScreen:
      return VK::kSnapshot;
    case ImGuiKey_Pause:
      return VK::kPause;
    case ImGuiKey_GraveAccent:
      return VK::kOem3;
    case ImGuiKey_Minus:
      return VK::kOemMinus;
    case ImGuiKey_Equal:
      return VK::kOemPlus;
    case ImGuiKey_LeftBracket:
      return VK::kOem4;
    case ImGuiKey_RightBracket:
      return VK::kOem6;
    case ImGuiKey_Backslash:
      return VK::kOem5;
    case ImGuiKey_Semicolon:
      return VK::kOem1;
    case ImGuiKey_Apostrophe:
      return VK::kOem7;
    case ImGuiKey_Comma:
      return VK::kOemComma;
    case ImGuiKey_Period:
      return VK::kOemPeriod;
    case ImGuiKey_Slash:
      return VK::kOem2;
    case ImGuiKey_KeypadDecimal:
      return VK::kDecimal;
    case ImGuiKey_KeypadDivide:
      return VK::kDivide;
    case ImGuiKey_KeypadMultiply:
      return VK::kMultiply;
    case ImGuiKey_KeypadSubtract:
      return VK::kSubtract;
    case ImGuiKey_KeypadAdd:
      return VK::kAdd;
    case ImGuiKey_KeypadEnter:
      return VK::kReturn;
    default:
      return VK::kNone;
  }
}

void SettingsDialog::OnDraw(ImGuiIO& io) {
  auto& registry = rex::cvar::GetRegistry();
  const auto inline_toggles = settings_rows::ResolveInlineToggles(registry, layout_);
  const Style& ui_style = imgui_drawer()->style();
  const SettingsStyle& settings_style = ui_style.settings;
  const float scale = imgui_drawer()->overlay_scale();
  const bool show_advanced = REXCVAR_GET(settings_show_advanced);
  const bool show_developer = REXCVAR_GET(settings_show_developer);

  const std::string search(search_buf_);
  const bool searching = !search.empty();

  // Visible rows in registration order. The advanced toggle appears only when
  // it would show something.
  std::vector<const rex::cvar::FlagEntry*> rows;
  bool has_advanced = false;
  for (auto& entry : registry) {
    const auto audience = settings_rows::EffectiveAudience(entry, layout_);
    if (settings_rows::IsVisibleRow(entry, layout_, inline_toggles, show_advanced,
                                    show_developer)) {
      rows.push_back(&entry);
    }
    has_advanced =
        has_advanced || (audience == rex::cvar::Audience::kAdvanced &&
                         settings_rows::IsVisibleRow(entry, layout_, inline_toggles, true, false));
  }

  // Unsaved rows compare against the values at open or the last save.
  std::vector<const rex::cvar::FlagEntry*> changed;
  for (auto& entry : registry) {
    auto it = saved_values_.find(entry.name);
    if (it != saved_values_.end() && settings_rows::IsUnsavedChange(entry, it->second)) {
      changed.push_back(&entry);
    }
  }

  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(760.0f * scale, 520.0f * scale), ImGuiCond_FirstUseEver);
  bool open = true;
  const bool was_capturing = !capturing_bind_name_.empty();
  if (!BeginOverlayWindow(ui_style, "Settings##rex", close_callback_ ? &open : nullptr,
                          ImGuiWindowFlags_NoCollapse, 0.92f)) {
    ImGui::End();
    return;
  }
  // Escape cancels a key capture or closes a popup first.
  if (close_callback_ && !was_capturing &&
      ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive() &&
      !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
      ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    open = false;
  }

  // Search bar at the top (full width).
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputTextWithHint("##search", "Search settings", search_buf_, sizeof(search_buf_));

  const float footer_height = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
  const float panel_width = 200.0f * scale;
  bool capture_row_drawn = false;
  float label_width = 0.0f;
  // A command may change the registry, so it runs after the frame stops using
  // registry entries.
  std::string clicked_command;

  auto draw_value = [&](const rex::cvar::FlagEntry& entry) {
    const std::string current_val = entry.getter();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (entry.type == rex::cvar::FlagType::Boolean) {
      bool v = rex::string::from_string<bool>(current_val, false);
      if (ImGui::Checkbox("##v", &v)) {
        rex::cvar::SetFlagByName(entry.name, v ? "true" : "false");
      }
    } else if (entry.type == rex::cvar::FlagType::String &&
               !entry.constraints.allowed_values.empty()) {
      const auto& opts = entry.constraints.allowed_values;
      int cur_idx = 0;
      for (int i = 0; i < static_cast<int>(opts.size()); ++i) {
        if (opts[i] == current_val) {
          cur_idx = i;
          break;
        }
      }
      const std::string preview = settings_rows::ChoiceLabel(entry, opts[cur_idx], layout_);
      if (ImGui::BeginCombo("##v", preview.c_str())) {
        for (int i = 0; i < static_cast<int>(opts.size()); ++i) {
          bool sel = (i == cur_idx);
          ImGui::PushID(i);
          const bool picked = OverlaySelectable(
              ui_style, settings_rows::ChoiceLabel(entry, opts[i], layout_).c_str(), sel);
          ImGui::PopID();
          if (picked) {
            rex::cvar::SetFlagByName(entry.name, opts[i]);
          }
          if (sel)
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
    } else if (const auto int_choices = settings_rows::IntegerChoices(entry, layout_);
               !int_choices.empty()) {
      const size_t cur_idx = settings_rows::IntegerChoiceIndex(int_choices, current_val);
      const std::string preview = settings_rows::IntegerChoicePreview(int_choices, current_val);
      if (ImGui::BeginCombo("##v", preview.c_str())) {
        for (size_t i = 0; i < int_choices.size(); ++i) {
          const bool sel = (i == cur_idx);
          ImGui::PushID(static_cast<int>(i));
          const bool picked = OverlaySelectable(ui_style, int_choices[i].label.c_str(), sel);
          ImGui::PopID();
          if (picked) {
            rex::cvar::SetFlagByName(entry.name, std::to_string(int_choices[i].value));
          }
          if (sel)
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
    } else if (entry.type == rex::cvar::FlagType::Int32) {
      InputInteger<int32_t>(entry, current_val, ImGuiDataType_S32);
    } else if (entry.type == rex::cvar::FlagType::Int64) {
      InputInteger<int64_t>(entry, current_val, ImGuiDataType_S64);
    } else if (entry.type == rex::cvar::FlagType::Uint32) {
      InputInteger<uint32_t>(entry, current_val, ImGuiDataType_U32);
    } else if (entry.type == rex::cvar::FlagType::Uint64) {
      InputInteger<uint64_t>(entry, current_val, ImGuiDataType_U64);
    } else if (entry.type == rex::cvar::FlagType::Double) {
      double v = std::atof(current_val.c_str());
      const std::string format = settings_rows::FloatFormat(entry, layout_);
      if (ImGui::InputDouble("##v", &v, 0.0, 0.0, format.c_str())) {
        if (entry.constraints.min)
          v = std::max(v, *entry.constraints.min);
        if (entry.constraints.max)
          v = std::min(v, *entry.constraints.max);
        const std::string edited = layout_.float_decimals.contains(entry.name)
                                       ? settings_rows::FloatEditText(v)
                                       : std::to_string(v);
        rex::cvar::SetFlagByName(entry.name, edited);
      }
    } else {
      char buf[1024];
      rex::string::copy_truncating(buf, current_val, sizeof(buf));
      const bool enter =
          ImGui::InputText("##v", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue);
      if (enter || ImGui::IsItemDeactivatedAfterEdit()) {
        rex::cvar::SetFlagByName(entry.name, buf);
      }
    }
  };

  auto draw_row = [&](const rex::cvar::FlagEntry& entry) {
    const auto* pair = settings_rows::FindInlineToggle(entry, inline_toggles);
    const auto tooltip_text = settings_rows::RowTooltipText(entry, layout_, pair);
    const auto tooltip_plan = settings_rows::PlanRowTooltip(entry.lifecycle, layout_, tooltip_text);
    // A command row is a single button; it changes no value, so it is never
    // saved and never counts as an unsaved change.
    if (entry.type == rex::cvar::FlagType::Command) {
      ImGui::PushID(entry.name.c_str());
      const std::string label = settings_rows::Label(entry, layout_);
      if (OverlayButton(ui_style, (label + "##v").c_str())) {
        clicked_command = entry.name;
      }
      if (ImGui::IsItemHovered()) {
        RowTooltip(entry, settings_style, tooltip_plan);
      }
      ImGui::PopID();
      return;
    }
    const bool read_only = (entry.lifecycle == rex::cvar::Lifecycle::kInitOnly);
    const std::string label = settings_rows::Label(entry, layout_);
    const std::string current_val = entry.getter();

    ImGui::PushID(entry.name.c_str());
    if (read_only)
      ImGui::BeginDisabled();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label.c_str());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      RowTooltip(entry, settings_style, tooltip_plan);
    ImGui::SameLine(label_width);

    if (pair) {
      // The bool and value retain separate lifecycle locks, as separate rows do.
      draw_value(entry);
      if (read_only)
        ImGui::EndDisabled();
      const bool checked = rex::string::from_string<bool>(entry.getter(), false);
      ImGui::SameLine();
      ImGui::PushID(pair->value->name.c_str());
      ImGui::BeginDisabled(!settings_rows::InlineValueEnabled(*pair, checked));
      if (settings_rows::InlineValueShown(*pair, checked)) {
        draw_value(*pair->value);
      } else {
        char blank[1] = {};
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##v", blank, sizeof(blank), ImGuiInputTextFlags_ReadOnly);
      }
      // Custom text stays on the label; only default details reach the field.
      if (!tooltip_text && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        RowTooltip(*pair->value, settings_style,
                   settings_rows::PlanRowTooltip(pair->value->lifecycle, layout_));
      }
      ImGui::EndDisabled();
      ImGui::PopID();
      ImGui::PopID();
      return;
    } else if (IsKeybindCategory(entry.category)) {
      // Grey out controller keybinds when MnK mode is disabled
      const bool mnk_disabled =
          (entry.category == "Input/Keybinds/Controller" && !REXCVAR_QUERY(bool, mnk_mode));
      if (mnk_disabled)
        ImGui::BeginDisabled();

      const ImVec2 bind_size(160.0f * scale, 0.0f);
      if (capturing_bind_name_ == entry.name) {
        capture_row_drawn = true;
        OverlayButton(ui_style, "Press a key...##v", bind_size);

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
          capturing_bind_name_.clear();
        } else {
          for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
            auto imgui_key = static_cast<ImGuiKey>(k);
            if (imgui_key == ImGuiKey_Escape)
              continue;
            if (ImGui::IsKeyPressed(imgui_key)) {
              auto vk = ImGuiKeyToVirtualKey(imgui_key);
              std::string name = rex::ui::VirtualKeyToString(vk);
              if (!name.empty()) {
                rex::cvar::SetFlagByName(entry.name, name);
              }
              capturing_bind_name_.clear();
              break;
            }
          }
          for (int mb = 0; mb < 3; ++mb) {
            if (ImGui::IsMouseClicked(mb)) {
              const char* names[] = {"LMB", "RMB", "MMB"};
              rex::cvar::SetFlagByName(entry.name, names[mb]);
              capturing_bind_name_.clear();
              break;
            }
          }
        }
      } else {
        const std::string shown =
            current_val.empty() ? "(none)" : settings_rows::FormatBinding(current_val);
        if (OverlayButton(ui_style, (shown + "##v").c_str(), bind_size)) {
          capturing_bind_name_ = entry.name;
          capture_row_drawn = true;
        }
        if (!tooltip_text && ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Click, then press a key");
        }
        if (current_val != entry.default_value) {
          ImGui::SameLine();
          if (OverlayButton(ui_style, "Reset##v", ImVec2(0, 0), true)) {
            rex::cvar::SetFlagByName(entry.name, entry.default_value);
          }
        }
      }

      // Conflict detection
      if (!current_val.empty()) {
        int conflict_count = 0;
        for (auto& other : registry) {
          if (IsKeybindCategory(other.category) && other.name != entry.name &&
              other.getter() == current_val) {
            conflict_count++;
          }
        }
        if (conflict_count > 0) {
          ImGui::SameLine();
          ImGui::TextColored(settings_style.warning, "(!)");
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Also bound to %d other action%s", conflict_count,
                              conflict_count == 1 ? "" : "s");
          }
        }
      }

      if (mnk_disabled)
        ImGui::EndDisabled();
    } else {
      draw_value(entry);
    }

    if (read_only)
      ImGui::EndDisabled();
    ImGui::PopID();
  };

  auto header_text = [](std::string text) {
    for (size_t pos = 0; (pos = text.find('/', pos)) != std::string::npos; pos += 3) {
      text.replace(pos, 1, " / ");
    }
    return text;
  };

  auto matches_search = [&](const rex::cvar::FlagEntry& entry) {
    const auto* pair = settings_rows::FindInlineToggle(entry, inline_toggles);
    return pair ? settings_rows::MatchesSearch(*pair, search, layout_)
                : settings_rows::MatchesSearch(entry, search, layout_);
  };

  if (!layout_.tabs.empty()) {
    const auto tabs = settings_rows::PlaceRows(rows, layout_);
    // Hiding advanced rows can remove the selected tab.
    if (std::none_of(tabs.begin(), tabs.end(),
                     [this](const auto& tab) { return tab.title == selected_category_; })) {
      selected_category_ = tabs.empty() ? std::string() : tabs.front().title;
    }

    if (BeginOverlayPanel(ui_style, "##tabs", ImVec2(panel_width, -footer_height))) {
      for (const auto& tab : tabs) {
        if (OverlaySelectable(ui_style, tab.title.c_str(),
                              !searching && selected_category_ == tab.title)) {
          selected_category_ = tab.title;
          search_buf_[0] = '\0';
        }
      }
    }
    EndOverlayPanel();
    ImGui::SameLine();

    if (BeginOverlayPanel(ui_style, "##cvars", ImVec2(0, -footer_height))) {
      label_width = std::max(220.0f * scale, ImGui::GetContentRegionAvail().x * 0.45f);
      size_t drawn = 0;
      for (const auto& tab : tabs) {
        if (!searching && tab.title != selected_category_) {
          continue;
        }
        for (size_t section_index = 0; section_index < tab.sections.size(); ++section_index) {
          const auto& section = tab.sections[section_index];
          bool header_drawn = false;
          bool section_open = true;
          for (const auto* row : section.rows) {
            if (searching && !matches_search(*row)) {
              continue;
            }
            if (!header_drawn) {
              header_drawn = true;
              const std::string title =
                  searching
                      ? (section.title.empty() ? tab.title : tab.title + " / " + section.title)
                      : section.title;
              // Titles may repeat, so the header ID uses the tab and section position.
              ImGui::PushID(tab.title.c_str());
              ImGui::PushID(static_cast<int>(section_index));
              if (section.collapsed && !searching && !title.empty()) {
                section_open = ImGui::CollapsingHeader(title.c_str());
              } else if (!title.empty()) {
                ImGui::SeparatorText(title.c_str());
              }
              ImGui::PopID();
              ImGui::PopID();
            }
            if (!section_open) {
              break;
            }
            ++drawn;
            draw_row(*row);
          }
        }
      }
      if (drawn == 0 && searching) {
        ImGui::TextDisabled("No settings match the search.");
      }
    }
    EndOverlayPanel();
  } else {
    std::stable_sort(rows.begin(), rows.end(),
                     [](const auto* a, const auto* b) { return a->category < b->category; });

    // Build tree: for each category path like "Input/Keybinds/Controller",
    // also register the parent paths "Input" and "Input/Keybinds" as nodes.
    struct CatNode {
      std::string full_path;
      std::string label;  // leaf segment (e.g. "Controller")
      std::map<std::string, CatNode> children;
    };
    std::map<std::string, CatNode> tree;
    std::set<std::string> node_paths;
    for (const auto* row : rows) {
      const std::string& cat = row->category;
      std::map<std::string, CatNode>* level = &tree;
      std::string path_so_far;
      size_t start = 0;
      while (start < cat.size()) {
        size_t slash = cat.find('/', start);
        std::string segment =
            (slash == std::string::npos) ? cat.substr(start) : cat.substr(start, slash - start);
        if (!path_so_far.empty())
          path_so_far += "/";
        path_so_far += segment;
        auto& node = (*level)[segment];
        node.full_path = path_so_far;
        node.label = segment;
        node_paths.insert(path_so_far);
        level = &node.children;
        start = (slash == std::string::npos) ? cat.size() : slash + 1;
      }
    }
    // Hiding advanced rows can remove the selected category.
    if (!selected_category_.empty() && !node_paths.contains(selected_category_)) {
      selected_category_.clear();
    }

    if (BeginOverlayPanel(ui_style, "##cats", ImVec2(panel_width, -footer_height))) {
      if (OverlaySelectable(ui_style, "All", selected_category_.empty() && !searching)) {
        selected_category_.clear();
        search_buf_[0] = '\0';
      }

      std::function<void(const std::map<std::string, CatNode>&, int)> draw_tree;
      draw_tree = [&](const std::map<std::string, CatNode>& nodes, int depth) {
        for (auto& [key, node] : nodes) {
          const bool selected = !searching && selected_category_ == node.full_path;
          if (node.children.empty()) {
            if (depth > 0)
              ImGui::Indent(8.0f * scale);
            if (OverlaySelectable(ui_style, node.label.c_str(), selected)) {
              selected_category_ = node.full_path;
              search_buf_[0] = '\0';
            }
            if (depth > 0)
              ImGui::Unindent(8.0f * scale);
          } else {
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen |
                                       ImGuiTreeNodeFlags_OpenOnArrow |
                                       ImGuiTreeNodeFlags_SpanAvailWidth;
            if (selected)
              flags |= ImGuiTreeNodeFlags_Selected;
            bool node_open = ImGui::TreeNodeEx(node.label.c_str(), flags);
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
              selected_category_ = node.full_path;
              search_buf_[0] = '\0';
            }
            if (node_open) {
              draw_tree(node.children, depth + 1);
              ImGui::TreePop();
            }
          }
        }
      };
      draw_tree(tree, 0);
    }
    EndOverlayPanel();

    ImGui::SameLine();

    // Exact match or prefix match (e.g. selecting "Input" shows all "Input/*").
    auto category_matches = [&](const std::string& cat) -> bool {
      if (selected_category_.empty())
        return true;
      if (cat == selected_category_)
        return true;
      return cat.size() > selected_category_.size() &&
             cat.compare(0, selected_category_.size(), selected_category_) == 0 &&
             cat[selected_category_.size()] == '/';
    };

    if (BeginOverlayPanel(ui_style, "##cvars", ImVec2(0, -footer_height))) {
      label_width = std::max(220.0f * scale, ImGui::GetContentRegionAvail().x * 0.45f);
      const std::string* last_category = nullptr;
      size_t drawn = 0;
      for (const auto* entry_ptr : rows) {
        const auto& entry = *entry_ptr;
        if (searching ? !matches_search(entry) : !category_matches(entry.category)) {
          continue;
        }
        ++drawn;
        if (!last_category || *last_category != entry.category) {
          ImGui::SeparatorText(header_text(entry.category).c_str());
          last_category = &entry.category;
        }
        draw_row(entry);
      }
      if (drawn == 0) {
        ImGui::TextDisabled(
            "%s", searching ? "No settings match the search." : "No settings in this category.");
      }
    }
    EndOverlayPanel();
  }
  // A capture whose row left the view (search, tab or advanced toggle)
  // would otherwise swallow Escape and the next key press.
  if (!capture_row_drawn) {
    capturing_bind_name_.clear();
  }

  if (OverlayButton(ui_style, "Save")) {
    save_failed_ = !rex::cvar::TrySaveConfig(config_path_);
    if (!save_failed_) {
      SnapshotSavedValues();
    }
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Write to %s", config_path_.filename().string().c_str());
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(changed.empty());
  if (OverlayButton(ui_style, "Revert")) {
    for (const auto* entry : changed) {
      rex::cvar::SetFlagByName(entry->name, saved_values_[entry->name]);
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  if (save_failed_) {
    ImGui::TextColored(settings_style.warning, "Could not save %s",
                       config_path_.filename().string().c_str());
  } else if (changed.empty()) {
    ImGui::TextDisabled("No unsaved changes");
  } else {
    ImGui::Text("%zu unsaved change%s", changed.size(), changed.size() == 1 ? "" : "s");
  }

  if (has_advanced) {
    const char* advanced_label = "Show advanced settings";
    const float advanced_width = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
                                 ImGui::CalcTextSize(advanced_label).x;
    ImGui::SameLine();
    const float advanced_offset = ImGui::GetContentRegionAvail().x - advanced_width;
    if (advanced_offset > 0.0f) {
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + advanced_offset);
    }
    bool advanced = show_advanced;
    if (ImGui::Checkbox(advanced_label, &advanced)) {
      rex::cvar::SetFlagByName("settings_show_advanced", advanced ? "true" : "false");
    }
  }

  ImGui::End();

  if (!clicked_command.empty()) {
    rex::cvar::InvokeCommand(clicked_command, "");
  }
  if (!open && close_callback_) {
    close_callback_();
  }
}

}  // namespace rex::ui
