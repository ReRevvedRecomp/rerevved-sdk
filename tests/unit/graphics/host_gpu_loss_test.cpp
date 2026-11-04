#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <rex/graphics/graphics_system.h>
#include <rex/ui/windowed_app_context.h>

namespace rex::graphics {
namespace {

class RecordingContext final : public ui::WindowedAppContext {
 public:
  std::atomic<int> notifications{0};
  int quits = 0;
  std::vector<std::string> events;

  void RequestWindowClosesFromUIThread() override {
    events.push_back(IsInUIThread() ? "close" : "close-off-ui-thread");
  }

 protected:
  void NotifyUILoopOfPendingFunctions() override { ++notifications; }
  void PlatformQuitFromUIThread() override { ++quits; }
};

class DefaultCloseContext final : public ui::WindowedAppContext {
 public:
  int quits = 0;

 protected:
  void NotifyUILoopOfPendingFunctions() override {}
  void PlatformQuitFromUIThread() override { ++quits; }
};

class BareGraphicsSystem final : public GraphicsSystem {
 public:
  std::string name() const override { return "bare"; }

  void Attach(ui::WindowedAppContext* context, std::vector<std::string>* events) {
    app_context_ = context;
    host_gpu_loss_notifier_ = [context, events](std::string_view message) {
      events->push_back(context->IsInUIThread() ? "message:" + std::string(message)
                                                : "message-off-ui-thread");
    };
  }

 protected:
  void CreateProvider(bool) override {}
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override { return {}; }
};

constexpr const char* kExpectedMessage =
    "message:The graphics driver stopped responding, so the game cannot continue and will now "
    "close.\n\nPlease restart the game. If this keeps happening, update your graphics driver.";

}  // namespace

TEST_CASE("a host GPU loss tells the player, then closes through the close-request route",
          "[graphics][gpu_loss]") {
  RecordingContext context;
  BareGraphicsSystem graphics;
  graphics.Attach(&context, &context.events);
  CHECK_FALSE(graphics.host_gpu_lost());

  graphics.OnHostGpuLossFromAnyThread(true);

  CHECK(graphics.host_gpu_lost());
  CHECK(context.notifications == 1);
  CHECK(context.events.empty());

  context.ExecutePendingFunctionsFromUIThread();

  REQUIRE(context.events.size() == 2);
  CHECK(context.events[0] == kExpectedMessage);
  CHECK(context.events[1] == "close");
  CHECK(context.quits == 0);
}

TEST_CASE("only the first host GPU loss report hands off, from any thread",
          "[graphics][gpu_loss]") {
  RecordingContext context;
  BareGraphicsSystem graphics;
  graphics.Attach(&context, &context.events);

  std::thread command_processor([&] { graphics.OnHostGpuLossFromAnyThread(false); });
  command_processor.join();
  graphics.OnHostGpuLossFromAnyThread(true);
  graphics.OnHostGpuLossFromAnyThread(false);
  std::thread presenter([&] { graphics.OnHostGpuLossFromAnyThread(true); });
  presenter.join();

  CHECK(context.notifications == 1);
  context.ExecutePendingFunctionsFromUIThread();
  REQUIRE(context.events.size() == 2);
  CHECK(context.events[0] == kExpectedMessage);
  CHECK(context.events[1] == "close");

  graphics.OnHostGpuLossFromAnyThread(true);
  CHECK(context.notifications == 1);
  context.ExecutePendingFunctionsFromUIThread();
  CHECK(context.events.size() == 2);
}

TEST_CASE("a handoff already posted survives the graphics system", "[graphics][gpu_loss]") {
  RecordingContext context;
  {
    BareGraphicsSystem graphics;
    graphics.Attach(&context, &context.events);
    graphics.OnHostGpuLossFromAnyThread(true);
  }

  context.ExecutePendingFunctionsFromUIThread();

  REQUIRE(context.events.size() == 2);
  CHECK(context.events[0] == kExpectedMessage);
  CHECK(context.events[1] == "close");
}

TEST_CASE("a handoff that runs after the UI loop quit does nothing", "[graphics][gpu_loss]") {
  RecordingContext context;
  BareGraphicsSystem graphics;
  graphics.Attach(&context, &context.events);
  graphics.OnHostGpuLossFromAnyThread(true);

  context.QuitFromUIThread();

  CHECK(context.events.empty());
  CHECK(context.quits == 1);
}

TEST_CASE("a report once the UI loop ended is dropped without a message", "[graphics][gpu_loss]") {
  RecordingContext context;
  BareGraphicsSystem graphics;
  graphics.Attach(&context, &context.events);
  context.QuitFromUIThread();

  graphics.OnHostGpuLossFromAnyThread(true);
  context.ExecutePendingFunctionsFromUIThread();

  CHECK(graphics.host_gpu_lost());
  CHECK(context.events.empty());
  CHECK(context.notifications == 0);
}

TEST_CASE("the default close hook quits the loop", "[graphics][gpu_loss]") {
  DefaultCloseContext context;
  context.RequestWindowClosesFromUIThread();
  CHECK(context.quits == 1);
}

}  // namespace rex::graphics
