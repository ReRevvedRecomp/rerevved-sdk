/**
 * @file        gamepad_nav_test.cpp
 * @brief       Pad navigation of the interactive overlay windows
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include <rex/runtime.h>
#include <rex/system/achievement_manager.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/overlay/achievements_overlay.h>
#include <rex/ui/overlay/mod_manager_overlay.h>
#include <rex/ui/overlay/window_chrome.h>
#include <rex/ui/overlay/settings_overlay.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>

#include "ui/gamepad_nav.h"

namespace {

using rex::input::X_INPUT_GAMEPAD;
using rex::input::X_INPUT_GAMEPAD_A;
using rex::input::X_INPUT_GAMEPAD_B;
using rex::input::X_INPUT_GAMEPAD_DPAD_DOWN;
using rex::input::X_INPUT_GAMEPAD_DPAD_UP;
using rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER;
using rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER;
using rex::input::X_INPUT_GAMEPAD_START;
using rex::input::X_INPUT_GAMEPAD_X;
using rex::input::X_INPUT_GAMEPAD_Y;
using rex::ui::GamepadNav;

class TestContext final : public rex::ui::WindowedAppContext {
 protected:
  void NotifyUILoopOfPendingFunctions() override {}
  void PlatformQuitFromUIThread() override {}
};

class TestWindow final : public rex::ui::Window {
 public:
  explicit TestWindow(TestContext& context) : Window(context, "pad test", 1280, 720) {}
  ~TestWindow() override { EnterDestructor(); }

 protected:
  bool OpenImpl() override { return false; }
  void RequestCloseImpl() override {}
  std::unique_ptr<rex::ui::Surface> CreateSurfaceImpl(rex::ui::Surface::TypeFlags) override {
    return nullptr;
  }
  void RequestPaintImpl() override {}
};

class TestDrawer final : public rex::ui::ImmediateDrawer {
 public:
  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(uint32_t, uint32_t,
                                                           rex::ui::ImmediateTextureFilter, bool,
                                                           const uint8_t*) override {
    return nullptr;
  }
  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch&) override {}
  void Draw(const rex::ui::ImmediateDraw&) override {}
  void EndDrawBatch() override {}
};

class TestDrawContext final : public rex::ui::UIDrawContext {
 public:
  TestDrawContext() : UIDrawContext(1280, 720) {}
};

X_INPUT_GAMEPAD Pad(uint16_t buttons = 0, int16_t lx = 0, int16_t ly = 0) {
  X_INPUT_GAMEPAD pad = {};
  pad.buttons = buttons;
  pad.thumb_lx = lx;
  pad.thumb_ly = ly;
  return pad;
}

/// Steps the drawer's ImGui context one frame at a time.
class Harness {
 public:
  Harness() : window_(context_), drawer_(&window_, 64) {
    auto& io = drawer_.GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    REQUIRE(io.Fonts->Build());
  }

  rex::ui::ImGuiDrawer& drawer() { return drawer_; }
  GamepadNav& nav() { return nav_; }
  ImGuiIO& io() { return drawer_.GetIO(); }

  /// Feeds a nav frame like ImGuiDrawer::Draw.
  void Frame(bool wanted, const X_INPUT_GAMEPAD& pad, const std::function<void()>& ui = {}) {
    auto& io = drawer_.GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    nav_.Update(io, wanted, &pad);
    ImGui::NewFrame();
    if (ui) {
      ui();
    }
    ImGui::EndFrame();
  }

  void Frames(int count, bool wanted, const X_INPUT_GAMEPAD& pad,
              const std::function<void()>& ui = {}) {
    for (int i = 0; i < count; ++i) {
      Frame(wanted, pad, ui);
    }
  }

  void NoPad(int count, bool wanted, const std::function<void()>& ui = {}) {
    auto& io = drawer_.GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    for (int i = 0; i < count; ++i) {
      nav_.Update(io, wanted, nullptr);
      ImGui::NewFrame();
      if (ui) {
        ui();
      }
      ImGui::EndFrame();
    }
  }

 private:
  TestContext context_;
  TestWindow window_;
  rex::ui::ImGuiDrawer drawer_;
  GamepadNav nav_;
};

struct TwoButtons {
  ImGuiID first = 0;
  ImGuiID second = 0;
  int first_pressed = 0;
  int second_pressed = 0;
  bool face_right_pressed = false;
  bool close_pressed = false;

  std::function<void()> Ui() {
    return [this] {
      face_right_pressed |= ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
      close_pressed |= rex::ui::OverlayPadClosePressed();
      ImGui::SetNextWindowPos(ImVec2(100, 100));
      ImGui::SetNextWindowSize(ImVec2(400, 300));
      ImGui::Begin("pad window");
      first_pressed += ImGui::Button("One") ? 1 : 0;
      first = ImGui::GetItemID();
      second_pressed += ImGui::Button("Two") ? 1 : 0;
      second = ImGui::GetItemID();
      ImGui::End();
    };
  }
};

bool Down(ImGuiKey key) {
  return ImGui::IsKeyDown(key);
}

}  // namespace

TEST_CASE("Pad buttons map to ImGui navigation keys", "[ui][gamepad]") {
  Harness h;
  h.Frame(true, Pad());
  h.Frame(true, Pad(X_INPUT_GAMEPAD_DPAD_DOWN | X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_Y |
                    X_INPUT_GAMEPAD_LEFT_SHOULDER | X_INPUT_GAMEPAD_RIGHT_SHOULDER));
  CHECK(Down(ImGuiKey_GamepadDpadDown));
  CHECK(Down(ImGuiKey_GamepadFaceDown));
  CHECK(Down(ImGuiKey_GamepadFaceUp));
  CHECK(Down(ImGuiKey_GamepadL1));
  CHECK(Down(ImGuiKey_GamepadR1));
  CHECK_FALSE(Down(ImGuiKey_GamepadDpadUp));

  // X would start ImGui's windowing mode and Start has no use here.
  h.Frame(true, Pad(X_INPUT_GAMEPAD_X | X_INPUT_GAMEPAD_START));
  CHECK_FALSE(Down(ImGuiKey_GamepadFaceLeft));
  CHECK_FALSE(Down(ImGuiKey_GamepadStart));
  CHECK_FALSE(Down(ImGuiKey_GamepadFaceDown));
}

TEST_CASE("The left stick steps focus like the D-pad, past the dead zone", "[ui][gamepad]") {
  Harness h;
  h.Frame(true, Pad());

  // The pad's up is positive; the stronger axis wins.
  h.Frame(true, Pad(0, 32767, 20000));
  CHECK(Down(ImGuiKey_GamepadDpadRight));
  CHECK_FALSE(Down(ImGuiKey_GamepadDpadUp));
  CHECK_FALSE(Down(ImGuiKey_GamepadDpadLeft));
  CHECK_FALSE(Down(ImGuiKey_GamepadDpadDown));

  h.Frame(true, Pad(0, -5000, 32767));
  CHECK(Down(ImGuiKey_GamepadDpadUp));
  CHECK_FALSE(Down(ImGuiKey_GamepadDpadLeft));

  h.Frame(true, Pad(0, -20000, -10000));
  CHECK(Down(ImGuiKey_GamepadDpadLeft));

  h.Frame(true, Pad(0, 1000, -32767));
  CHECK(Down(ImGuiKey_GamepadDpadDown));

  h.Frame(true, Pad(0, 7000, -7000));
  CHECK_FALSE(Down(ImGuiKey_GamepadDpadRight));
  CHECK_FALSE(Down(ImGuiKey_GamepadDpadDown));

  // Scrolling keys stay unsent: the stick would otherwise scroll and step.
  h.Frame(true, Pad(0, 32767, 0));
  CHECK_FALSE(Down(ImGuiKey_GamepadLStickRight));
}

TEST_CASE("Tilting the stick moves focus", "[ui][gamepad]") {
  Harness h;
  TwoButtons ui;
  h.Frames(3, true, Pad(), ui.Ui());
  h.Frames(2, true, Pad(0, 0, -32767), ui.Ui());
  h.Frames(2, true, Pad(), ui.Ui());
  CHECK(ImGui::GetCurrentContext()->NavId == ui.second);
  h.Frames(2, true, Pad(0, 0, 32767), ui.Ui());
  h.Frames(2, true, Pad(), ui.Ui());
  CHECK(ImGui::GetCurrentContext()->NavId == ui.first);
}

TEST_CASE("Navigation follows the wanted flag and releases its keys", "[ui][gamepad]") {
  Harness h;
  CHECK((h.io().ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) == 0);

  h.Frame(true, Pad());
  CHECK((h.io().ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) != 0);
  CHECK((h.io().BackendFlags & ImGuiBackendFlags_HasGamepad) != 0);

  h.Frame(true, Pad(X_INPUT_GAMEPAD_DPAD_DOWN));
  REQUIRE(Down(ImGuiKey_GamepadDpadDown));

  h.Frame(false, Pad(X_INPUT_GAMEPAD_DPAD_DOWN));
  CHECK_FALSE(Down(ImGuiKey_GamepadDpadDown));
  CHECK((h.io().ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) == 0);
  CHECK((h.io().BackendFlags & ImGuiBackendFlags_HasGamepad) == 0);
}

TEST_CASE("A button held when navigation starts counts only after release", "[ui][gamepad]") {
  Harness h;
  TwoButtons ui;
  h.Frames(3, true, Pad(X_INPUT_GAMEPAD_A), ui.Ui());
  CHECK(ui.first_pressed == 0);
  CHECK_FALSE(Down(ImGuiKey_GamepadFaceDown));

  h.Frames(2, true, Pad(), ui.Ui());
  CHECK(ui.first_pressed == 0);

  h.Frame(true, Pad(X_INPUT_GAMEPAD_A), ui.Ui());
  CHECK(ui.first_pressed == 1);
}

TEST_CASE("A pad that arrives with a button held waits for its release", "[ui][gamepad]") {
  Harness h;
  TwoButtons ui;
  h.NoPad(3, true, ui.Ui());
  h.Frames(2, true, Pad(X_INPUT_GAMEPAD_A), ui.Ui());
  CHECK(ui.first_pressed == 0);
  h.Frames(2, true, Pad(), ui.Ui());
  h.Frame(true, Pad(X_INPUT_GAMEPAD_A), ui.Ui());
  CHECK(ui.first_pressed == 1);

  // Pad switches appear as one disconnected read.
  h.Frames(2, true, Pad(), ui.Ui());
  h.NoPad(1, true, ui.Ui());
  h.Frames(2, true, Pad(X_INPUT_GAMEPAD_A), ui.Ui());
  CHECK(ui.first_pressed == 1);
}

TEST_CASE("A window opening re-shows a focus highlight the mouse hid", "[ui][gamepad]") {
  Harness h;
  TwoButtons ui;
  h.Frames(3, true, Pad(), ui.Ui());
  ImGuiContext& g = *ImGui::GetCurrentContext();
  ImGui::SetNavCursorVisible(false);
  h.Frames(2, true, Pad(), ui.Ui());
  CHECK_FALSE(g.NavCursorVisible);

  h.nav().NotifyWindowOpened();
  h.Frame(true, Pad(), ui.Ui());
  CHECK(g.NavCursorVisible);
}

TEST_CASE("A new window takes nav focus with a visible highlight on its first item",
          "[ui][gamepad]") {
  Harness h;
  TwoButtons ui;
  h.Frames(3, true, Pad(), ui.Ui());
  ImGuiContext& g = *ImGui::GetCurrentContext();
  CHECK(g.NavId == ui.first);
  CHECK(g.NavCursorVisible);
  CHECK(g.NavWindow != nullptr);
}

TEST_CASE("D-pad moves focus and A activates", "[ui][gamepad]") {
  Harness h;
  TwoButtons ui;
  h.Frames(3, true, Pad(), ui.Ui());
  h.Frames(2, true, Pad(X_INPUT_GAMEPAD_DPAD_DOWN), ui.Ui());
  h.Frames(2, true, Pad(), ui.Ui());
  CHECK(ImGui::GetCurrentContext()->NavId == ui.second);

  h.Frames(2, true, Pad(X_INPUT_GAMEPAD_A), ui.Ui());
  h.Frames(2, true, Pad(), ui.Ui());
  CHECK(ui.second_pressed == 1);
  CHECK(ui.first_pressed == 0);

  h.Frames(2, true, Pad(X_INPUT_GAMEPAD_DPAD_UP), ui.Ui());
  h.Frames(2, true, Pad(), ui.Ui());
  CHECK(ImGui::GetCurrentContext()->NavId == ui.first);
}

TEST_CASE("B is ImGui's cancel key and never an injected Escape", "[ui][gamepad]") {
  Harness h;
  TwoButtons ui;
  h.Frames(3, true, Pad(), ui.Ui());

  h.Frame(true, Pad(X_INPUT_GAMEPAD_B), ui.Ui());
  CHECK(ui.face_right_pressed);
  CHECK(ui.close_pressed);
  CHECK_FALSE(Down(ImGuiKey_Escape));
  CHECK(Down(ImGuiKey_GamepadFaceRight));

  h.Frame(true, Pad(), ui.Ui());
  CHECK_FALSE(Down(ImGuiKey_GamepadFaceRight));
}

TEST_CASE("A physically held Escape is left alone by the pad", "[ui][gamepad]") {
  Harness h;
  TwoButtons ui;
  h.Frames(3, true, Pad(), ui.Ui());
  h.io().AddKeyEvent(ImGuiKey_Escape, true);
  h.Frame(true, Pad(X_INPUT_GAMEPAD_B), ui.Ui());
  h.Frame(true, Pad(), ui.Ui());
  CHECK(Down(ImGuiKey_Escape));
}

TEST_CASE("B held when navigation starts is not a press", "[ui][gamepad]") {
  Harness h;
  TwoButtons ui;
  h.Frames(4, true, Pad(X_INPUT_GAMEPAD_B), ui.Ui());
  CHECK_FALSE(ui.face_right_pressed);
}

TEST_CASE("B backs out of an open popup before it closes the window", "[ui][gamepad]") {
  Harness h;
  TwoButtons ui;
  bool popup_opened = false;
  bool popup_visible = false;
  auto with_popup = [&] {
    auto inner = ui.Ui();
    inner();
    ImGui::SetNextWindowPos(ImVec2(600, 100));
    ImGui::Begin("popup host");
    if (!popup_opened) {
      ImGui::OpenPopup("choices");
      popup_opened = true;
    }
    popup_visible = ImGui::BeginPopup("choices");
    if (popup_visible) {
      ImGui::Selectable("a");
      ImGui::EndPopup();
    }
    ImGui::End();
  };
  h.Frames(3, true, Pad(), with_popup);
  REQUIRE(popup_visible);

  h.Frame(true, Pad(X_INPUT_GAMEPAD_B), with_popup);
  CHECK(ui.face_right_pressed);
  h.Frame(true, Pad(X_INPUT_GAMEPAD_B), with_popup);
  CHECK_FALSE(popup_visible);

  // Holding B after popup closure must not close the window.
  h.Frames(3, true, Pad(X_INPUT_GAMEPAD_B), with_popup);
  CHECK_FALSE(ui.close_pressed);

  h.Frames(2, true, Pad(), with_popup);
  h.Frame(true, Pad(X_INPUT_GAMEPAD_B), with_popup);
  CHECK(ui.close_pressed);
}

TEST_CASE("B answers a message box", "[ui][gamepad]") {
  Harness h;
  auto* box = rex::ui::ImGuiDialog::ShowMessageBox(&h.drawer(), "Title", "Body");
  REQUIRE(h.drawer().WantsGamepad());
  const auto ui = [&] {
    if (h.drawer().WantsGamepad()) {
      box->Draw();
    }
  };
  h.Frames(4, true, Pad(), ui);
  REQUIRE(h.drawer().WantsGamepad());

  h.Frames(2, true, Pad(X_INPUT_GAMEPAD_B), ui);
  CHECK_FALSE(h.drawer().WantsGamepad());
}

namespace {

void Tap(Harness& h, uint16_t button, const std::function<void()>& ui) {
  h.Frames(2, true, Pad(button), ui);
  h.Frames(2, true, Pad(), ui);
}

const char* NavWindowName() {
  ImGuiWindow* window = ImGui::GetCurrentContext()->NavWindow;
  return window ? window->Name : "";
}

}  // namespace

TEST_CASE("Mods window: focus, LB/RB tabs and B close", "[ui][gamepad]") {
  Harness h;
  int closes = 0;
  rex::Runtime runtime{std::filesystem::path{}};
  auto dialog = std::make_unique<rex::ui::ModManagerDialog>(&h.drawer(), nullptr, &runtime,
                                                            [&closes] { ++closes; });
  CHECK(h.drawer().WantsGamepad());
  auto ui = [&dialog] { dialog->Draw(); };

  // Models A held from selecting the menu row.
  h.Frames(4, h.drawer().WantsGamepad(), Pad(X_INPUT_GAMEPAD_A), ui);
  ImGuiContext& g = *ImGui::GetCurrentContext();
  CHECK(std::string(NavWindowName()) == "Mods##overlay");
  CHECK(g.NavId != 0);
  CHECK(g.NavCursorVisible);
  h.Frames(2, true, Pad(), ui);

  ImGuiWindow* window = ImGui::FindWindowByName("Mods##overlay");
  REQUIRE(window != nullptr);
  ImGuiTabBar* bar = g.TabBars.GetByKey(window->GetID("##package-tabs"));
  REQUIRE(bar != nullptr);
  REQUIRE(bar->Tabs.Size == 2);
  CHECK(bar->SelectedTabId == bar->Tabs[0].ID);

  Tap(h, X_INPUT_GAMEPAD_RIGHT_SHOULDER, ui);
  CHECK(bar->SelectedTabId == bar->Tabs[1].ID);
  Tap(h, X_INPUT_GAMEPAD_LEFT_SHOULDER, ui);
  CHECK(bar->SelectedTabId == bar->Tabs[0].ID);
  CHECK(closes == 0);

  h.Frames(3, true, Pad(X_INPUT_GAMEPAD_B), ui);
  CHECK(closes == 1);
  h.Frames(2, true, Pad(), ui);
  CHECK(closes == 1);
}

TEST_CASE("Settings window is navigable by pad and closes with B", "[ui][gamepad]") {
  Harness h;
  int closes = 0;
  auto dialog = std::make_unique<rex::ui::SettingsDialog>(
      &h.drawer(), std::filesystem::path("pad_test.toml"), [&closes] { ++closes; });
  auto ui = [&dialog] { dialog->Draw(); };

  h.Frames(4, true, Pad(), ui);
  ImGuiContext& g = *ImGui::GetCurrentContext();
  CHECK(std::string(NavWindowName()).rfind("Settings##rex", 0) == 0);
  const ImGuiID first = g.NavId;
  CHECK(first != 0);
  // A on the search box would start typing, so focus starts elsewhere.
  ImGuiWindow* settings = ImGui::FindWindowByName("Settings##rex");
  REQUIRE(settings != nullptr);
  CHECK(first != settings->GetID("##search"));

  std::vector<ImGuiID> visited = {first};
  for (int i = 0; i < 12; ++i) {
    Tap(h, X_INPUT_GAMEPAD_DPAD_DOWN, ui);
    if (visited.back() != g.NavId) {
      visited.push_back(g.NavId);
    }
  }
  CHECK(visited.size() > 4);
  CHECK(std::string(NavWindowName()).rfind("Settings##rex", 0) == 0);

  h.Frames(3, true, Pad(X_INPUT_GAMEPAD_B), ui);
  CHECK(closes == 1);
}

TEST_CASE("Achievements: rows take focus, LB/RB switch tabs, B closes", "[ui][gamepad]") {
  rex::system::AchievementManager achievements;
  for (uint32_t id = 1; id <= 14; ++id) {
    rex::system::AchievementInfo info;
    info.id = id;
    info.label = "Achievement " + std::to_string(id);
    info.description = "Unlocked";
    info.unachieved_description = "Locked";
    info.gamerscore = 10;
    info.custom = id > 12;
    achievements.RegisterAchievement(info);
  }
  Harness h;
  int closes = 0;
  auto dialog = std::make_unique<rex::ui::AchievementsOverlayDialog>(
      &h.drawer(), nullptr, nullptr, &achievements, [&closes] { ++closes; });
  auto ui = [&dialog] { dialog->Draw(); };

  h.Frames(4, true, Pad(), ui);
  ImGuiContext& g = *ImGui::GetCurrentContext();
  CHECK(g.NavId != 0);
  const ImGuiID first = g.NavId;

  // Beyond the tab bar, only rows can take focus; each step must scroll to one.
  std::vector<ImGuiID> visited = {first};
  for (int i = 0; i < 8; ++i) {
    Tap(h, X_INPUT_GAMEPAD_DPAD_DOWN, ui);
    if (visited.back() != g.NavId) {
      visited.push_back(g.NavId);
    }
  }
  CHECK(visited.size() == 9);

  ImGuiWindow* window = ImGui::FindWindowByName("Achievements##overlay");
  REQUIRE(window != nullptr);
  ImGuiTabBar* bar = g.TabBars.GetByKey(window->GetID("##ach-tabs"));
  REQUIRE(bar != nullptr);
  REQUIRE(bar->Tabs.Size == 2);
  CHECK(bar->SelectedTabId == bar->Tabs[0].ID);
  Tap(h, X_INPUT_GAMEPAD_RIGHT_SHOULDER, ui);
  CHECK(bar->SelectedTabId == bar->Tabs[1].ID);
  Tap(h, X_INPUT_GAMEPAD_LEFT_SHOULDER, ui);
  CHECK(bar->SelectedTabId == bar->Tabs[0].ID);

  h.Frames(3, true, Pad(X_INPUT_GAMEPAD_B), ui);
  CHECK(closes == 1);
}

TEST_CASE("The drawer feeds the pad to navigation and releases it with the last window",
          "[ui][gamepad]") {
  TestContext context;
  TestWindow window(context);
  rex::ui::ImGuiDrawer drawer(&window, 64);
  TestDrawer immediate;
  drawer.SetImmediateDrawer(&immediate);
  TestDrawContext draw_context;
  auto& io = drawer.GetIO();

  X_INPUT_GAMEPAD pad = Pad();
  int polls = 0;
  drawer.SetGamepadSource([&](X_INPUT_GAMEPAD* out) {
    ++polls;
    *out = pad;
    return true;
  });

  class PlainDialog final : public rex::ui::ImGuiDialog {
   public:
    explicit PlainDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  };
  class PadDialog final : public rex::ui::ImGuiDialog {
   public:
    explicit PadDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer, true) {}
  };
  auto plain = std::make_unique<PlainDialog>(&drawer);
  CHECK_FALSE(drawer.WantsGamepad());
  drawer.Draw(draw_context);
  CHECK(polls == 0);
  CHECK((io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) == 0);

  auto navigated = std::make_unique<PadDialog>(&drawer);
  CHECK(drawer.WantsGamepad());
  drawer.Draw(draw_context);
  CHECK(polls == 1);
  CHECK(ImGui::GetCurrentContext()->NavCursorVisible);

  ImGui::SetNavCursorVisible(false);
  auto second = std::make_unique<PadDialog>(&drawer);
  drawer.Draw(draw_context);
  CHECK(ImGui::GetCurrentContext()->NavCursorVisible);
  second.reset();
  CHECK((io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) != 0);

  pad = Pad(X_INPUT_GAMEPAD_DPAD_DOWN);
  drawer.Draw(draw_context);
  CHECK(ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown));

  navigated.reset();
  CHECK_FALSE(drawer.WantsGamepad());
  drawer.Draw(draw_context);
  CHECK((io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) == 0);
  CHECK_FALSE(ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown));

  navigated = std::make_unique<PadDialog>(&drawer);
  drawer.Draw(draw_context);
  CHECK((io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) != 0);
  navigated.reset();
  plain.reset();
  CHECK((drawer.GetIO().ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) == 0);
  drawer.SetImmediateDrawer(nullptr);
}
