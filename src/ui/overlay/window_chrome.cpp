
/**
 * @file        ui/overlay/window_chrome.cpp
 *
 * @brief       Overlay window skin helpers. See window_chrome.h for details.
 */

#include <rex/ui/overlay/window_chrome.h>

#include <cstring>
#include <functional>
#include <initializer_list>

namespace rex::ui {

bool BeginOverlayWindow(const Style& style, const char* label, bool* p_open, ImGuiWindowFlags flags,
                        float background_alpha) {
  const ChromeStyle& chrome = style.chrome;
  if (!chrome.draw_window) {
    ImGui::SetNextWindowBgAlpha(background_alpha);
    return ImGui::Begin(label, p_open, flags);
  }

  const bool titled = (flags & ImGuiWindowFlags_NoTitleBar) == 0;
  ImGui::SetNextWindowBgAlpha(0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  const bool visible = ImGui::Begin(label, nullptr, flags | ImGuiWindowFlags_NoTitleBar);
  ImGui::PopStyleVar();
  if (!visible) {
    return false;
  }

  const float title_height = titled ? ImGui::GetFontSize() * chrome.title_height_em : 0.0f;
  const ImVec2 min = ImGui::GetWindowPos();
  const ImVec2 size = ImGui::GetWindowSize();
  const ImVec2 max(min.x + size.x, min.y + size.y);
  ImDrawList* draw_list = ImGui::GetWindowDrawList();
  // The window clip rect excludes padding; the skin covers the whole window.
  draw_list->PushClipRect(min, max, false);
  chrome.draw_window(draw_list, min, max, title_height);
  if (titled) {
    const char* hidden = std::strstr(label, "##");
    const ImVec2 text_pos(
        min.x + ImGui::GetStyle().WindowPadding.x + ImGui::GetFontSize() * chrome.title_indent_em,
        min.y + (title_height - ImGui::GetFontSize()) * 0.5f);
    draw_list->AddText(text_pos, ImGui::GetColorU32(ImGuiCol_Text), label, hidden);
  }
  draw_list->PopClipRect();

  if (titled) {
    const ImGuiStyle& imgui_style = ImGui::GetStyle();
    if (p_open) {
      const float button = ImGui::GetFrameHeight();
      ImGui::SetCursorPos(
          ImVec2(size.x - imgui_style.WindowPadding.x - button, (title_height - button) * 0.5f));
      if (OverlayButton(style, "X##rex_chrome_close", ImVec2(button, button))) {
        *p_open = false;
      }
    }
    ImGui::SetCursorPos(
        ImVec2(imgui_style.WindowPadding.x, title_height + imgui_style.WindowPadding.y));
  }
  return true;
}

bool BeginOverlayPanel(const Style& style, const char* id, ImVec2 size, ImGuiWindowFlags flags) {
  const ChromeStyle& chrome = style.chrome;
  if (!chrome.draw_panel) {
    return ImGui::BeginChild(id, size, ImGuiChildFlags_Borders, flags);
  }
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
  const bool visible = ImGui::BeginChild(id, size, ImGuiChildFlags_AlwaysUseWindowPadding, flags);
  ImGui::PopStyleColor();
  if (visible) {
    const ImVec2 min = ImGui::GetWindowPos();
    const ImVec2 child_size = ImGui::GetWindowSize();
    const ImVec2 max(min.x + child_size.x, min.y + child_size.y);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->PushClipRect(min, max, false);
    chrome.draw_panel(draw_list, min, max);
    draw_list->PopClipRect();
  }
  return visible;
}

void EndOverlayPanel() {
  ImGui::EndChild();
}

namespace {

using SkinCallback = std::function<void(ImDrawList*, ImVec2, ImVec2, ChromeState)>;

// Draws the widget's label on a second channel, then the skin underneath it
// once the item's rect and state are known. A local splitter nests safely
// inside tables and other channel users.
template <typename Widget>
bool DrawOverSkin(const SkinCallback& skin, std::initializer_list<ImGuiCol> hidden, bool selected,
                  Widget widget) {
  ImDrawList* draw_list = ImGui::GetWindowDrawList();
  ImDrawListSplitter splitter;
  splitter.Split(draw_list, 2);
  splitter.SetCurrentChannel(draw_list, 1);
  for (ImGuiCol col : hidden) {
    ImGui::PushStyleColor(col, IM_COL32(0, 0, 0, 0));
  }
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  const bool pressed = widget();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(static_cast<int>(hidden.size()));
  splitter.SetCurrentChannel(draw_list, 0);
  ChromeState state = ChromeState::kNormal;
  if (ImGui::IsItemActive()) {
    state = ChromeState::kActive;
  } else if (selected) {
    state = ChromeState::kSelected;
  } else if (ImGui::IsItemHovered()) {
    state = ChromeState::kHovered;
  }
  skin(draw_list, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), state);
  splitter.Merge(draw_list);
  return pressed;
}

}  // namespace

bool OverlayButton(const Style& style, const char* label, ImVec2 size, bool compact) {
  auto widget = [&] { return compact ? ImGui::SmallButton(label) : ImGui::Button(label, size); };
  if (!style.chrome.draw_button) {
    return widget();
  }
  return DrawOverSkin(style.chrome.draw_button,
                      {ImGuiCol_Button, ImGuiCol_ButtonHovered, ImGuiCol_ButtonActive}, false,
                      widget);
}

bool OverlaySelectable(const Style& style, const char* label, bool selected,
                       ImGuiSelectableFlags flags, ImVec2 size) {
  auto widget = [&] { return ImGui::Selectable(label, selected, flags, size); };
  // Spanning rows restore their column clip before returning, so the skin
  // could not cover the whole row; they keep ImGui's colored highlight.
  if (!style.chrome.draw_selection || (flags & ImGuiSelectableFlags_SpanAllColumns)) {
    return widget();
  }
  const SkinCallback& draw = style.chrome.draw_selection;
  return DrawOverSkin(
      [&draw](ImDrawList* draw_list, ImVec2 min, ImVec2 max, ChromeState state) {
        if (state != ChromeState::kNormal) {
          draw(draw_list, min, max, state);
        }
      },
      {ImGuiCol_Header, ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive}, selected, widget);
}

}  // namespace rex::ui
