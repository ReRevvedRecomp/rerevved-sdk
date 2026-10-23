/**
 * @file        detached_presentation_test.cpp
 * @brief       Detached overlay ownership and application close tests
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <rex/input/input_system.h>
#include <rex/rex_app.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/keybinds.h>

namespace rex {

// Keep Runtime::Setup (guest address-space reservation) and native windows out
// of these tests. The input overload runs the same teardown as the public hook.
struct ReXAppTestAccess {
  static void SetDrawers(ReXApp& app, std::unique_ptr<ui::Window> window,
                         std::unique_ptr<ui::ImmediateDrawer> drawer) {
    app.window_ = std::move(window);
    app.immediate_drawer_ = std::move(drawer);
    app.imgui_drawer_ = std::make_unique<ui::ImGuiDrawer>(app.window_.get(), 64);
    app.imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, app.immediate_drawer_.get());
  }

  static void SetOverlay(ReXApp& app, std::unique_ptr<ui::ImGuiDialog> dialog) {
    app.achievements_overlay_ = std::move(dialog);
  }

  static void SetRuntime(ReXApp& app) {
    app.runtime_ = std::make_unique<Runtime>(std::filesystem::path{});
    app.runtime_->set_imgui_drawer(app.imgui_drawer_.get());
  }

  static void SetWindow(ReXApp& app, std::unique_ptr<ui::Window> window,
                        std::unique_ptr<ui::ImmediateDrawer> drawer) {
    app.window_ = std::move(window);
    app.immediate_drawer_ = std::move(drawer);
  }

  static void SetupOverlays(ReXApp& app) {
    app.SetupOverlays(nullptr, app.immediate_drawer_.get());
  }

  static bool HasFpsCounter(const ReXApp& app) { return app.fps_counter_overlay_ != nullptr; }

  static void SetGraphics(ReXApp& app, std::unique_ptr<system::IGraphicsSystem> graphics) {
    app.config_.graphics = std::move(graphics);
  }

  static void ShutdownOverlays(ReXApp& app, input::InputSystem* input) {
    app.ShutdownOverlays(input);
  }
};

}  // namespace rex

namespace {

using Access = rex::ReXAppTestAccess;

class TestContext final : public rex::ui::WindowedAppContext {
 public:
  int quit_count = 0;

 protected:
  void NotifyUILoopOfPendingFunctions() override {}
  void PlatformQuitFromUIThread() override { ++quit_count; }
};

class TestWindow final : public rex::ui::Window {
 public:
  explicit TestWindow(TestContext& context) : Window(context, "detached test", 1280, 720) {}
  ~TestWindow() override { EnterDestructor(); }

 protected:
  bool OpenImpl() override { return false; }
  void RequestCloseImpl() override {}
  std::unique_ptr<rex::ui::Surface> CreateSurfaceImpl(rex::ui::Surface::TypeFlags) override {
    return nullptr;
  }
  void RequestPaintImpl() override {}
};

class TestApp final : public rex::ReXApp {
 public:
  explicit TestApp(TestContext& context) : ReXApp(context, "detached test", {}) {}

  using ReXApp::imgui_drawer;
  using ReXApp::immediate_drawer;
  using ReXApp::IsOverlayInputCaptured;
  using ReXApp::runtime;

  void ShutdownOverlays() { ReXApp::ShutdownOverlays(); }

  bool accept_close = true;
  int shutdown_count = 0;
  int before_hard_exit_count = 0;

 protected:
  bool OnWindowCloseRequested() override { return accept_close; }
  void OnShutdown() override { ++shutdown_count; }
  void OnBeforeHardExit() override {
    ++before_hard_exit_count;
    // Intercept the runtime path before process termination, including if a
    // regression accidentally routes the no-runtime fixture through it.
    throw std::runtime_error("runtime hard exit reached");
  }
};

// Stands in for an SDK graphics backend; none of its methods is reached.
class TestGraphics final : public rex::system::IGraphicsSystem {
 public:
  rex::X_STATUS SetupPresentation(rex::ui::WindowedAppContext*) override { return 0; }
  rex::X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher*,
                              rex::system::KernelState*) override {
    return 0;
  }
  bool has_presentation() const override { return false; }
  void Shutdown() override {}
};

class TestDrawer final : public rex::ui::ImmediateDrawer {
 public:
  explicit TestDrawer(std::function<void()> on_destroy = {}) : on_destroy_(std::move(on_destroy)) {}
  ~TestDrawer() override {
    if (on_destroy_) {
      on_destroy_();
    }
  }

  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(uint32_t, uint32_t,
                                                           rex::ui::ImmediateTextureFilter, bool,
                                                           const uint8_t*) override {
    return nullptr;
  }
  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch&) override {}
  void Draw(const rex::ui::ImmediateDraw&) override {}
  void EndDrawBatch() override {}

 private:
  std::function<void()> on_destroy_;
};

class TestDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit TestDialog(rex::ui::ImGuiDrawer* drawer, std::function<void()> on_destroy = {})
      : ImGuiDialog(drawer), on_destroy_(std::move(on_destroy)) {}
  ~TestDialog() override {
    if (on_destroy_) {
      on_destroy_();
    }
  }

 private:
  std::function<void()> on_destroy_;
};

class TestInputDriver final : public rex::input::InputDriver {
 public:
  TestInputDriver() : InputDriver(nullptr, 0) {}
  bool IsActive() const { return is_active(); }

  rex::X_STATUS Setup() override { return 0; }
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>&) override {}
  rex::X_RESULT GetDeviceState(rex::input::DeviceId, rex::input::X_INPUT_STATE*) override {
    return 0;
  }
  rex::X_RESULT GetDeviceCapabilities(rex::input::DeviceId, uint32_t,
                                      rex::input::X_INPUT_CAPABILITIES*) override {
    return 0;
  }
  rex::X_RESULT SetDeviceVibration(rex::input::DeviceId, rex::input::X_INPUT_VIBRATION*) override {
    return 0;
  }
  rex::X_RESULT GetDeviceKeystroke(rex::input::DeviceId, uint32_t,
                                   rex::input::X_INPUT_KEYSTROKE*) override {
    return 0;
  }
};

bool DispatchKey(rex::ui::VirtualKey key) {
  rex::ui::KeyEvent event(nullptr, key, 1, false, false, false, false, false);
  return rex::ui::ProcessKeyEvent(event);
}

}  // namespace

TEST_CASE("Detached overlays shut down safely before setup", "[ui][detached]") {
  TestContext context;
  TestApp app(context);
  app.ShutdownOverlays();
  app.ShutdownOverlays();
  app.InvokeOnDestroy();
  CHECK(app.imgui_drawer() == nullptr);
  CHECK(app.immediate_drawer() == nullptr);
  CHECK_FALSE(app.IsOverlayInputCaptured());
  CHECK(app.shutdown_count == 1);
}

TEST_CASE("Detached overlays release borrowed access before destruction", "[ui][detached]") {
  TestContext context;
  rex::input::InputSystem input(nullptr);
  auto driver = std::make_unique<TestInputDriver>();
  auto* driver_ptr = driver.get();
  input.AddDriver(std::move(driver));
  int callback_calls = 0;
  int bind_calls = 0;
  std::vector<std::string> destroyed;
  bool callback_cleared = false;
  bool runtime_drawer_cleared = false;
  bool binds_cleared = false;
  bool imgui_destroyed_before_drawer = false;
  TestApp app(context);
  Access::SetDrawers(app, std::make_unique<TestWindow>(context), std::make_unique<TestDrawer>([&] {
                       imgui_destroyed_before_drawer = app.imgui_drawer() == nullptr;
                       destroyed.push_back("drawer");
                     }));
  Access::SetRuntime(app);

  const std::array binds = {
      std::pair{"bind_debug_overlay", "F3"}, std::pair{"bind_console", "Backtick"},
      std::pair{"bind_settings", "F4"},      std::pair{"bind_mod_manager", "F1"},
      std::pair{"bind_achievements", "F7"},  std::pair{"bind_fps_counter", "F12"}};
  for (const auto& [name, key] : binds) {
    rex::ui::RegisterBind(name, key, "Detached test bind", [&] { ++bind_calls; });
    CHECK(DispatchKey(rex::ui::ParseVirtualKey(key)));
  }
  CHECK(bind_calls == 6);

  Access::SetOverlay(app, std::make_unique<TestDialog>(app.imgui_drawer(), [&] {
                       callback_cleared = driver_ptr->IsActive() && callback_calls == 1;
                       runtime_drawer_cleared = app.runtime()->imgui_drawer() == nullptr;
                       binds_cleared = true;
                       for (const auto& [name, key] : binds) {
                         binds_cleared &= !DispatchKey(rex::ui::ParseVirtualKey(key));
                       }
                       destroyed.push_back("dialog");
                     }));
  app.imgui_drawer()->GetIO().WantCaptureKeyboard = true;
  input.SetActiveCallback([&] {
    ++callback_calls;
    return !app.IsOverlayInputCaptured();
  });
  CHECK_FALSE(driver_ptr->IsActive());

  Access::ShutdownOverlays(app, &input);
  CHECK(callback_cleared);
  CHECK(runtime_drawer_cleared);
  CHECK(binds_cleared);
  CHECK(bind_calls == 6);
  CHECK(imgui_destroyed_before_drawer);
  CHECK(destroyed == std::vector<std::string>{"dialog", "drawer"});
  CHECK(app.imgui_drawer() == nullptr);
  CHECK(app.immediate_drawer() == nullptr);

  // A second call must not clear a callback installed by the next owner.
  input.SetActiveCallback([] { return false; });
  Access::ShutdownOverlays(app, &input);
  app.ShutdownOverlays();
  app.InvokeOnDestroy();
  CHECK_FALSE(driver_ptr->IsActive());
  CHECK(destroyed.size() == 2);
  for (const auto& [name, key] : binds) {
    rex::cvar::UnregisterFlag(name);
  }
}

TEST_CASE("Detached overlay capture includes mouse and keyboard", "[ui][detached]") {
  TestContext context;
  TestApp app(context);
  CHECK_FALSE(app.IsOverlayInputCaptured());
  Access::SetDrawers(app, std::make_unique<TestWindow>(context), std::make_unique<TestDrawer>());

  for (bool attached : {false, true}) {
    // A title-owned dialog exercises the same predicate as built-in overlays.
    auto dialog = attached ? std::make_unique<TestDialog>(app.imgui_drawer()) : nullptr;
    for (bool mouse : {false, true}) {
      for (bool keyboard : {false, true}) {
        CAPTURE(attached, mouse, keyboard);
        auto& io = app.imgui_drawer()->GetIO();
        io.WantCaptureMouse = mouse;
        io.WantCaptureKeyboard = keyboard;
        CHECK(app.IsOverlayInputCaptured() == (attached && (mouse || keyboard)));
      }
    }
  }
  // Capture flags can outlive the last dialog until the next ImGui frame.
  CHECK_FALSE(app.IsOverlayInputCaptured());
  app.ShutdownOverlays();
  CHECK_FALSE(app.IsOverlayInputCaptured());
}

TEST_CASE("ImGui drawer processes modifier down and up events across frames", "[ui][detached]") {
  TestContext context;
  TestWindow window(context);
  rex::ui::ImGuiDrawer drawer(&window, 64);
  auto& listener = static_cast<rex::ui::WindowInputListener&>(drawer);
  auto& io = drawer.GetIO();
  io.DisplaySize = ImVec2(1280, 720);
  REQUIRE(io.Fonts->Build());
  struct Modifier {
    rex::ui::VirtualKey key;
    ImGuiKey modifier;
    bool ImGuiIO::* state;
  };
  const Modifier modifiers[] = {
      {rex::ui::VirtualKey::kControl, ImGuiMod_Ctrl, &ImGuiIO::KeyCtrl},
      {rex::ui::VirtualKey::kShift, ImGuiMod_Shift, &ImGuiIO::KeyShift},
      {rex::ui::VirtualKey::kMenu, ImGuiMod_Alt, &ImGuiIO::KeyAlt},
      {rex::ui::VirtualKey::kLWin, ImGuiMod_Super, &ImGuiIO::KeySuper},
  };
  for (const auto& modifier : modifiers) {
    CAPTURE(modifier.modifier);
    for (bool down : {true, false}) {
      CAPTURE(down);
      rex::ui::KeyEvent event(&window, modifier.key, 1, false, false, false, false, false);
      if (down) {
        listener.OnKeyDown(event);
      } else {
        listener.OnKeyUp(event);
      }
      ImGui::NewFrame();
      CHECK(io.*modifier.state == down);
      CHECK(ImGui::IsKeyDown(modifier.modifier) == down);
      ImGui::EndFrame();
    }
  }
}

TEST_CASE("Detached no-runtime close quits normally", "[ui][detached]") {
  TestContext context;
  TestApp app(context);
  rex::ui::UIEvent event;
  auto& listener = static_cast<rex::ui::WindowListener&>(app);
  bool pending_ran = false;
  context.CallInUIThreadDeferred([&] { pending_ran = true; });

  REQUIRE_NOTHROW(listener.OnClosing(event));
  CHECK(context.HasQuitFromUIThread());
  CHECK(context.quit_count == 1);
  CHECK(pending_ran);
  CHECK(app.before_hard_exit_count == 0);
  CHECK(app.shutdown_count == 0);
  app.InvokeOnDestroy();
  CHECK(app.shutdown_count == 1);
}

TEST_CASE("Detached runtime close offers a final cleanup hook", "[ui][detached]") {
  TestContext context;
  TestApp app(context);
  Access::SetRuntime(app);
  rex::ui::UIEvent event;
  auto& listener = static_cast<rex::ui::WindowListener&>(app);

  app.accept_close = false;
  CHECK_FALSE(listener.OnCloseRequested(event));
  CHECK(app.before_hard_exit_count == 0);
  app.accept_close = true;
  CHECK(listener.OnCloseRequested(event));
  CHECK_THROWS_AS(listener.OnClosing(event), std::runtime_error);
  CHECK(app.before_hard_exit_count == 1);
  CHECK_FALSE(context.HasQuitFromUIThread());
  app.InvokeOnDestroy();
}

TEST_CASE("Presenter-backed close without a runtime keeps the hard exit", "[ui][detached]") {
  TestContext context;
  TestApp app(context);
  Access::SetGraphics(app, std::make_unique<TestGraphics>());
  rex::ui::UIEvent event;
  auto& listener = static_cast<rex::ui::WindowListener&>(app);

  // The hook throws just before the process exit, so reaching it proves the
  // runtime-less presenter-backed close did not take the ordinary quit path.
  CHECK_THROWS_AS(listener.OnClosing(event), std::runtime_error);
  CHECK(app.before_hard_exit_count == 1);
  CHECK_FALSE(context.HasQuitFromUIThread());
  CHECK(context.quit_count == 0);
}

TEST_CASE("Overlay setup re-arms shutdown after an early call", "[ui][detached]") {
  TestContext context;
  TestApp app(context);
  Access::SetWindow(app, std::make_unique<TestWindow>(context), std::make_unique<TestDrawer>());

  app.ShutdownOverlays();
  Access::SetupOverlays(app);
  REQUIRE(app.imgui_drawer() != nullptr);

  const std::array binds = {
      std::pair{"bind_debug_overlay", "F3"}, std::pair{"bind_console", "Backtick"},
      std::pair{"bind_settings", "F4"},      std::pair{"bind_mod_manager", "F1"},
      std::pair{"bind_achievements", "F7"},  std::pair{"bind_fps_counter", "F12"}};
  for (const auto& [name, key] : binds) {
    CHECK(DispatchKey(rex::ui::ParseVirtualKey(key)));
  }

  app.InvokeOnDestroy();
  CHECK(app.imgui_drawer() == nullptr);
  CHECK(app.immediate_drawer() == nullptr);
  for (const auto& [name, key] : binds) {
    CHECK_FALSE(DispatchKey(rex::ui::ParseVirtualKey(key)));
    rex::cvar::UnregisterFlag(name);
  }
  // F12 toggled the counter on; keep later tests on the default.
  CHECK(rex::cvar::GetFlagByName("show_fps_counter") == "true");
  rex::cvar::ResetToDefault("show_fps_counter");
}

TEST_CASE("Queued FPS counter work does not outlive overlay shutdown", "[ui][detached]") {
  TestContext context;
  auto app = std::make_unique<TestApp>(context);
  Access::SetWindow(*app, std::make_unique<TestWindow>(context), std::make_unique<TestDrawer>());
  Access::SetupOverlays(*app);
  CHECK_FALSE(Access::HasFpsCounter(*app));

  REQUIRE(rex::cvar::SetFlagByName("show_fps_counter", "true"));
  context.ExecutePendingFunctionsFromUIThread();
  CHECK(Access::HasFpsCounter(*app));

  // Queue a change, then tear the app down before the UI loop runs it.
  REQUIRE(rex::cvar::SetFlagByName("show_fps_counter", "false"));
  app->InvokeOnDestroy();
  CHECK_FALSE(Access::HasFpsCounter(*app));
  app.reset();
  context.ExecutePendingFunctionsFromUIThread();

  for (const char* name : {"bind_debug_overlay", "bind_console", "bind_settings",
                           "bind_mod_manager", "bind_achievements", "bind_fps_counter"}) {
    rex::cvar::UnregisterFlag(name);
  }
  rex::cvar::ResetToDefault("show_fps_counter");
}

TEST_CASE("Application bind defaults replace registered defaults", "[ui][keybinds]") {
  int calls = 0;
  rex::ui::SetBindDefault("bind_app_default_test", "F9");
  rex::ui::RegisterBind("bind_app_default_test", "F3", "App default test bind", [&] { ++calls; });

  CHECK(DispatchKey(rex::ui::VirtualKey::kF9));
  CHECK_FALSE(DispatchKey(rex::ui::VirtualKey::kF3));
  CHECK(calls == 1);
  const auto* info = rex::cvar::GetFlagInfo("bind_app_default_test");
  REQUIRE(info != nullptr);
  CHECK(info->default_value == "F9");
  CHECK_FALSE(rex::cvar::HasNonDefaultValue("bind_app_default_test"));

  // A saved key still wins over the application default.
  rex::cvar::SetFlagByName("bind_app_default_test", "F11");
  CHECK(DispatchKey(rex::ui::VirtualKey::kF11));
  CHECK(calls == 2);

  rex::ui::UnregisterBind("bind_app_default_test");
  rex::cvar::UnregisterFlag("bind_app_default_test");
}
