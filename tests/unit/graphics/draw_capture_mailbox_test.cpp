#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <rex/graphics/draw_capture_mailbox.h>

namespace rex::graphics::diagnostic {
namespace {

TEST_CASE("Draw capture shares immutable vertex input ownership", "[graphics][capture]") {
  DrawCaptureVertexFetch fetch;
  fetch.shared_bytes = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{1, 2, 3});
  auto retained = fetch;
  fetch.shared_bytes.reset();
  fetch.bytes = {4, 5};
  CHECK(retained.Payload().size() == 3);
  CHECK(retained.Payload()[0] == 1);
  CHECK(fetch.Payload().size() == 2);
  CHECK(fetch.Payload()[0] == 4);
}

DrawCaptureRequest MakeRequest(const char* path, uint64_t vertex_shader_hash = 1,
                               uint64_t pixel_shader_hash = 2) {
  DrawCaptureRequest request;
  request.output_path = path;
  request.vertex_shader_hash = vertex_shader_hash;
  request.pixel_shader_hash = pixel_shader_hash;
  return request;
}

std::shared_ptr<const DrawCaptureSnapshot> MakeSnapshot(uint64_t vertex_shader_hash = 1,
                                                        uint64_t pixel_shader_hash = 2) {
  auto snapshot = std::make_shared<DrawCaptureSnapshot>();
  snapshot->vertex_shader_hash = vertex_shader_hash;
  snapshot->pixel_shader_hash = pixel_shader_hash;
  snapshot->registers = {0x5003};
  snapshot->vertex_fetches.push_back({95, 0x100, {0x01, 0x02, 0x03, 0x04}});
  return snapshot;
}

std::shared_ptr<const DrawCaptureFrameSnapshot> MakeFrameSnapshot() {
  auto snapshot = std::make_shared<DrawCaptureFrameSnapshot>();
  snapshot->start_frame = 10;
  snapshot->end_frame = 11;
  snapshot->events.push_back({});
  snapshot->events.back().kind = DrawCaptureFrameEvent::Kind::kSwap;
  return snapshot;
}

class InputObserver final : public DrawCaptureFrameInputObserver {
 public:
  bool OnFrameBegin(const DrawCaptureFrameInputSwapView& boundary) override {
    ++begin_count;
    last_swap_id = boundary.id;
    return !fail;
  }
  bool OnDraw(const DrawCaptureFrameInputDrawView& draw) override {
    ++draw_count;
    last_draw_id = draw.id;
    if (block_draw) {
      std::unique_lock lock(mutex);
      draw_entered = true;
      entered.notify_all();
      released.wait(lock, [this] { return release_draw; });
    }
    if (throw_on_draw) {
      throw std::runtime_error("observer failure");
    }
    if (on_draw) {
      on_draw();
    }
    return !fail;
  }
  bool OnCopy(const DrawCaptureFrameInputCopyView& copy) override {
    ++copy_count;
    last_copy_original_succeeded = copy.original_succeeded;
    last_copy_written_address = copy.written_address;
    last_copy_written_length = copy.written_length;
    return !fail;
  }
  bool OnSwap(const DrawCaptureFrameInputSwapView& swap) override {
    ++swap_count;
    last_swap_id = swap.id;
    return !fail;
  }
  bool OnFrameEnd(uint64_t, uint64_t, bool) override {
    ++end_count;
    return !fail;
  }

  void WaitForDraw() {
    std::unique_lock lock(mutex);
    entered.wait(lock, [this] { return draw_entered; });
  }
  void ReleaseDraw() {
    std::lock_guard lock(mutex);
    release_draw = true;
    released.notify_all();
  }

  bool fail = false;
  bool block_draw = false;
  bool throw_on_draw = false;
  std::function<void()> on_draw;
  std::atomic<uint32_t> begin_count = 0;
  std::atomic<uint32_t> draw_count = 0;
  std::atomic<uint32_t> copy_count = 0;
  std::atomic<uint32_t> swap_count = 0;
  std::atomic<uint32_t> end_count = 0;
  std::atomic<bool> last_copy_original_succeeded = false;
  std::atomic<uint32_t> last_copy_written_address = 0;
  std::atomic<uint32_t> last_copy_written_length = 0;
  std::atomic<uint64_t> last_draw_id = 0;
  std::atomic<uint64_t> last_swap_id = 0;

 private:
  std::mutex mutex;
  std::condition_variable entered;
  std::condition_variable released;
  bool draw_entered = false;
  bool release_draw = false;
};

TEST_CASE("draw capture mailbox requires a complete selector") {
  DrawCaptureMailbox mailbox;

  CHECK_FALSE(mailbox.Start({}).value);
  CHECK_FALSE(mailbox.Start(MakeRequest("capture", 0, 2)).value);
  CHECK_FALSE(mailbox.Start(MakeRequest("capture", 1, 0)).value);
  CHECK_FALSE(mailbox.Start(MakeRequest("", 1, 2)).value);
}

TEST_CASE("draw capture mailbox hands off one owned snapshot") {
  DrawCaptureMailbox mailbox;
  const DrawCaptureRequest request = MakeRequest("capture");
  const DrawCaptureMailbox::Token token = mailbox.Start(request);
  REQUIRE(token);
  CHECK(mailbox.IsPending(token));
  CHECK_FALSE(mailbox.Start(request));
  CHECK_FALSE(mailbox.TryTake(token));
  CHECK_FALSE(mailbox.GetRequestToken("other"));
  CHECK(mailbox.GetRequestToken(request.output_path) == token);
  CHECK_FALSE(mailbox.Claim(token, "other", request.vertex_shader_hash, request.pixel_shader_hash));

  const DrawCaptureMailbox::Token claim = mailbox.Claim(
      token, request.output_path, request.vertex_shader_hash, request.pixel_shader_hash);
  REQUIRE(claim == token);
  std::shared_ptr<const DrawCaptureSnapshot> snapshot = MakeSnapshot();
  REQUIRE(mailbox.Publish(token, snapshot));
  snapshot.reset();

  const std::shared_ptr<const DrawCaptureSnapshot> taken = mailbox.TryTake(token);
  REQUIRE(taken);
  CHECK_FALSE(mailbox.IsPending(token));
  REQUIRE(taken->registers == std::vector<uint32_t>{0x5003});
  REQUIRE(taken->vertex_fetches.size() == 1);
  CHECK(taken->vertex_fetches[0].bytes == std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04});
  CHECK_FALSE(mailbox.TryTake(token));
  CHECK(mailbox.Start(request));
}

TEST_CASE("draw capture mailbox rejects stale producer completions") {
  DrawCaptureMailbox mailbox;
  const DrawCaptureRequest request = MakeRequest("capture");
  const DrawCaptureMailbox::Token stale_token = mailbox.Start(request);
  REQUIRE(stale_token);
  REQUIRE(mailbox.Cancel(stale_token));
  CHECK_FALSE(mailbox.IsPending(stale_token));
  CHECK_FALSE(mailbox.GetRequestToken(request.output_path));
  CHECK_FALSE(mailbox.Claim(stale_token, request.output_path, request.vertex_shader_hash,
                            request.pixel_shader_hash));

  const DrawCaptureMailbox::Token current_token = mailbox.Start(request);
  REQUIRE(current_token);
  REQUIRE(current_token != stale_token);
  CHECK_FALSE(mailbox.Claim(stale_token, request.output_path, request.vertex_shader_hash,
                            request.pixel_shader_hash));
  REQUIRE(mailbox.Claim(current_token, request.output_path, request.vertex_shader_hash,
                        request.pixel_shader_hash));
  CHECK_FALSE(mailbox.Publish(stale_token, MakeSnapshot()));
  CHECK_FALSE(mailbox.Fail(stale_token));
  REQUIRE(mailbox.Publish(current_token, MakeSnapshot()));
  REQUIRE(mailbox.TryTake(current_token));
}

TEST_CASE("draw capture mailbox failure releases the request") {
  DrawCaptureMailbox mailbox;
  const DrawCaptureRequest request = MakeRequest("capture");
  const DrawCaptureMailbox::Token token = mailbox.Start(request);
  REQUIRE(token);
  CHECK(mailbox.Fail(token));
  CHECK_FALSE(mailbox.IsPending(token));
  CHECK_FALSE(mailbox.TryTake(token));
  CHECK(mailbox.Start(request));
}

TEST_CASE("draw capture mailbox hands off an owned frame") {
  DrawCaptureMailbox mailbox;
  const DrawCaptureMailbox::Token token = mailbox.StartFrame("frame");
  REQUIRE(token);
  CHECK(mailbox.IsPending(token));
  CHECK(mailbox.IsFrameRequest(token));
  CHECK(mailbox.GetFrameRequestToken() == token);
  CHECK(mailbox.GetRequestPath(token) == std::filesystem::path("frame"));
  CHECK_FALSE(mailbox.TryTakeFrame(token));
  CHECK_FALSE(mailbox.ClaimFrame(token, "other"));
  REQUIRE(mailbox.ClaimFrame(token, "frame") == token);
  CHECK_FALSE(mailbox.GetFrameRequestToken());
  CHECK_FALSE(mailbox.ClaimFrame(token, "frame"));
  REQUIRE(mailbox.PublishFrame(token, MakeFrameSnapshot()));
  CHECK(mailbox.IsPending(token));
  CHECK_FALSE(mailbox.GetFrameRequestToken());
  CHECK_FALSE(mailbox.PublishFrame(token, MakeFrameSnapshot()));

  const std::shared_ptr<const DrawCaptureFrameSnapshot> snapshot = mailbox.TryTakeFrame(token);
  REQUIRE(snapshot);
  CHECK(snapshot->schema_version == kDrawCaptureFrameSchemaVersion);
  CHECK(snapshot->start_frame == 10);
  CHECK(snapshot->events.size() == 1);
  CHECK_FALSE(mailbox.IsPending(token));
  CHECK_FALSE(mailbox.GetFrameRequestToken());
  const DrawCaptureMailbox::Token next_token = mailbox.StartFrame("next-frame");
  REQUIRE(next_token);
  REQUIRE(mailbox.ClaimFrame(next_token, "next-frame") == next_token);
  REQUIRE(mailbox.PublishFrame(next_token, MakeFrameSnapshot()));
  REQUIRE(mailbox.TryTakeFrame(next_token));
}

TEST_CASE("draw capture mailbox rejects stale owned frame completion") {
  DrawCaptureMailbox mailbox;
  const DrawCaptureMailbox::Token stale_token = mailbox.StartFrame();
  REQUIRE(stale_token);
  REQUIRE(mailbox.Cancel(stale_token));
  CHECK_FALSE(mailbox.IsFrameRequest(stale_token));
  CHECK_FALSE(mailbox.ClaimFrame(stale_token));
  CHECK_FALSE(mailbox.PublishFrame(stale_token, MakeFrameSnapshot()));

  const DrawCaptureMailbox::Token current_token = mailbox.StartFrame();
  REQUIRE(current_token);
  REQUIRE(mailbox.ClaimFrame(current_token));
  CHECK_FALSE(mailbox.PublishFrame(stale_token, MakeFrameSnapshot()));
  REQUIRE(mailbox.PublishFrame(current_token, MakeFrameSnapshot()));
  REQUIRE(mailbox.TryTakeFrame(current_token));
}

TEST_CASE("draw capture mailbox streams live input views with a disabled fast path") {
  DrawCaptureMailbox mailbox;
  CHECK_FALSE(mailbox.HasFrameInputObserver());
  CHECK_FALSE(mailbox.DispatchFrameInputDraw({}, {}));

  auto observer = std::make_shared<InputObserver>();
  const DrawCaptureMailbox::ObserverToken token = mailbox.RegisterFrameInputObserver(observer);
  REQUIRE(token);
  CHECK(mailbox.HasFrameInputObserver());
  CHECK(mailbox.GetFrameInputObserverToken() == token);

  const uint32_t registers[] = {0x5003, 0x14000500};
  DrawCaptureFrameInputDrawView draw;
  draw.id = 7;
  draw.registers = registers;
  REQUIRE(mailbox.DispatchFrameInputDraw(token, draw));
  CHECK(observer->draw_count == 1);
  CHECK(observer->last_draw_id == 7);
  REQUIRE(mailbox.UnregisterFrameInputObserver(token));
  CHECK_FALSE(mailbox.HasFrameInputObserver());
  CHECK_FALSE(mailbox.DispatchFrameInputDraw(token, draw));
}

TEST_CASE("draw capture mailbox carries the original copy result and guest range") {
  DrawCaptureMailbox mailbox;
  auto observer = std::make_shared<InputObserver>();
  const DrawCaptureMailbox::ObserverToken token = mailbox.RegisterFrameInputObserver(observer);
  REQUIRE(token);

  DrawCaptureFrameInputCopyView copy;
  copy.original_succeeded = true;
  copy.written_address = 0x1000;
  copy.written_length = 0x200;
  REQUIRE(mailbox.DispatchFrameInputCopy(token, copy));
  CHECK(observer->last_copy_original_succeeded);
  CHECK(observer->last_copy_written_address == 0x1000);
  CHECK(observer->last_copy_written_length == 0x200);

  copy.original_succeeded = true;
  copy.written_address = 0;
  copy.written_length = 0;
  REQUIRE(mailbox.DispatchFrameInputCopy(token, copy));
  CHECK(observer->last_copy_original_succeeded);
  CHECK(observer->last_copy_written_address == 0);
  CHECK(observer->last_copy_written_length == 0);

  copy.original_succeeded = false;
  REQUIRE(mailbox.DispatchFrameInputCopy(token, copy));
  CHECK_FALSE(observer->last_copy_original_succeeded);
  CHECK(observer->last_copy_written_address == 0);
  CHECK(observer->last_copy_written_length == 0);
  REQUIRE(mailbox.UnregisterFrameInputObserver(token));
}

TEST_CASE("draw capture mailbox preserves live input boundaries across swaps") {
  DrawCaptureMailbox mailbox;
  auto observer = std::make_shared<InputObserver>();
  const DrawCaptureMailbox::ObserverToken token = mailbox.RegisterFrameInputObserver(observer);
  REQUIRE(token);

  DrawCaptureFrameInputSwapView first_boundary;
  first_boundary.id = 10;
  DrawCaptureFrameInputSwapView next_boundary;
  next_boundary.id = 20;
  REQUIRE(mailbox.DispatchFrameInputBegin(token, first_boundary));
  REQUIRE(mailbox.DispatchFrameInputSwap(token, first_boundary));

  DrawCaptureFrameInputDrawView draw;
  draw.id = 11;
  REQUIRE(mailbox.DispatchFrameInputDraw(token, draw));
  REQUIRE(mailbox.DispatchFrameInputEnd(token, 1, 1, true));
  REQUIRE(mailbox.DispatchFrameInputSwap(token, next_boundary));
  REQUIRE(mailbox.DispatchFrameInputEnd(token, 2, 2, true));

  CHECK(observer->begin_count == 1);
  CHECK(observer->swap_count == 2);
  CHECK(observer->draw_count == 1);
  CHECK(observer->end_count == 2);
  CHECK(observer->last_draw_id == draw.id);
  CHECK(observer->last_swap_id == next_boundary.id);
  CHECK(mailbox.UnregisterFrameInputObserver(token));
}

TEST_CASE("draw capture mailbox isolates callback failure and rejects stale observer tokens") {
  DrawCaptureMailbox mailbox;
  auto failing_observer = std::make_shared<InputObserver>();
  const DrawCaptureMailbox::ObserverToken stale =
      mailbox.RegisterFrameInputObserver(failing_observer);
  REQUIRE(stale);
  failing_observer->fail = true;
  CHECK_FALSE(mailbox.DispatchFrameInputCopy(stale, {}));
  CHECK_FALSE(mailbox.HasFrameInputObserver());
  CHECK_FALSE(mailbox.DispatchFrameInputCopy(stale, {}));

  auto throwing_observer = std::make_shared<InputObserver>();
  throwing_observer->throw_on_draw = true;
  const DrawCaptureMailbox::ObserverToken throwing =
      mailbox.RegisterFrameInputObserver(throwing_observer);
  REQUIRE(throwing);
  CHECK_FALSE(mailbox.DispatchFrameInputDraw(throwing, {}));
  CHECK_FALSE(mailbox.HasFrameInputObserver());

  auto current_observer = std::make_shared<InputObserver>();
  const DrawCaptureMailbox::ObserverToken current =
      mailbox.RegisterFrameInputObserver(current_observer);
  REQUIRE(current);
  CHECK(current != stale);
  CHECK_FALSE(mailbox.DispatchFrameInputCopy(stale, {}));
  REQUIRE(mailbox.DispatchFrameInputCopy(current, {}));
  REQUIRE(mailbox.UnregisterFrameInputObserver(current));
}

TEST_CASE("draw capture mailbox unregister waits for an in-flight callback") {
  DrawCaptureMailbox mailbox;
  auto observer = std::make_shared<InputObserver>();
  observer->block_draw = true;
  const DrawCaptureMailbox::ObserverToken token = mailbox.RegisterFrameInputObserver(observer);
  REQUIRE(token);

  std::atomic<bool> dispatched = false;
  std::thread dispatch_thread([&] { dispatched = mailbox.DispatchFrameInputDraw(token, {}); });
  observer->WaitForDraw();
  CHECK_FALSE(mailbox.DispatchFrameInputCopy(token, {}));
  CHECK(observer->copy_count == 0);

  std::atomic<bool> unregistered = false;
  std::thread unregister_thread(
      [&] { unregistered = mailbox.UnregisterFrameInputObserver(token); });
  std::this_thread::yield();
  CHECK_FALSE(unregistered.load());
  observer->ReleaseDraw();
  dispatch_thread.join();
  unregister_thread.join();
  CHECK(dispatched.load());
  CHECK(unregistered.load());
  CHECK_FALSE(mailbox.HasFrameInputObserver());
}

TEST_CASE("draw capture mailbox rejects recursive observer dispatch") {
  DrawCaptureMailbox mailbox;
  auto observer = std::make_shared<InputObserver>();
  const auto token = mailbox.RegisterFrameInputObserver(observer);
  REQUIRE(token);
  bool recursive_result = true;
  observer->on_draw = [&] { recursive_result = mailbox.DispatchFrameInputCopy(token, {}); };
  REQUIRE(mailbox.DispatchFrameInputDraw(token, {}));
  CHECK_FALSE(recursive_result);
  CHECK(observer->copy_count == 0);
  REQUIRE(mailbox.DispatchFrameInputCopy(token, {}));
  CHECK(observer->copy_count == 1);
  CHECK(mailbox.UnregisterFrameInputObserver(token));
}

}  // namespace
}  // namespace rex::graphics::diagnostic
