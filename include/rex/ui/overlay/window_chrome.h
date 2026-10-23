/**
 * @file        rex/ui/overlay/window_chrome.h
 *
 * @brief       Overlay window and panel helpers that apply the optional
 *              ChromeStyle skin.
 */
#pragma once
#include <imgui.h>
#include <rex/ui/style.h>

namespace rex::ui {

/// Begins a top-level overlay window. Without ChromeStyle::draw_window this is
/// ImGui::SetNextWindowBgAlpha(background_alpha) plus ImGui::Begin. With it,
/// the ImGui background, border and title bar are replaced by the callback,
/// and the title (the label up to "##") and close button are drawn in the
/// title band. Always pair with ImGui::End(), like ImGui::Begin.
bool BeginOverlayWindow(const Style& style, const char* label, bool* p_open, ImGuiWindowFlags flags,
                        float background_alpha);

/// Begins a padded child region drawn with ChromeStyle::draw_panel, or a
/// bordered child without it. Always pair with EndOverlayPanel().
bool BeginOverlayPanel(const Style& style, const char* id, ImVec2 size, ImGuiWindowFlags flags = 0);
void EndOverlayPanel();

/// ImGui::Button, or ImGui::SmallButton when compact is set, drawn over
/// ChromeStyle::draw_button when the skin has one.
bool OverlayButton(const Style& style, const char* label, ImVec2 size = ImVec2(0, 0),
                   bool compact = false);

/// ImGui::Selectable drawn over ChromeStyle::draw_selection when the skin has one.
/// Rows with ImGuiSelectableFlags_SpanAllColumns keep the plain highlight.
bool OverlaySelectable(const Style& style, const char* label, bool selected,
                       ImGuiSelectableFlags flags = 0, ImVec2 size = ImVec2(0, 0));

}  // namespace rex::ui
