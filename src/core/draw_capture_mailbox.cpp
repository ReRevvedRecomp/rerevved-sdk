#include <rex/graphics/draw_capture_mailbox.h>

#include <cassert>
#include <condition_variable>
#include <exception>
#include <utility>

namespace rex::graphics::diagnostic {

struct DrawCaptureMailbox::ObserverState {
  ObserverToken token;
  std::shared_ptr<DrawCaptureFrameInputObserver> observer;
  bool active = true;
  bool unregistering = false;
  uint32_t in_flight = 0;
  std::condition_variable quiesced;
};

namespace {

// A callback cannot synchronously unregister itself: doing so would wait for
// its own completion. It may request unregister, which disables the fast path;
// the dispatch epilogue then releases the final state reference.
thread_local void* callback_state = nullptr;

}  // namespace

std::shared_ptr<DrawCaptureMailbox> GetDrawCaptureMailbox() {
  static const std::shared_ptr<DrawCaptureMailbox> mailbox = std::make_shared<DrawCaptureMailbox>();
  return mailbox;
}

void DrawCaptureMailbox::ResetLocked() {
  active_token_ = {};
  request_ = {};
  frame_request_ = false;
  claimed_ = false;
  ready_.reset();
  ready_frame_.reset();
}

DrawCaptureMailbox::Token DrawCaptureMailbox::Start(const DrawCaptureRequest& request) {
  if (request.output_path.empty() || !request.vertex_shader_hash || !request.pixel_shader_hash) {
    return {};
  }
  std::lock_guard lock(mutex_);
  if (active_token_ || !next_token_) {
    return {};
  }
  // Copy before activating the request so allocation failure leaves it idle.
  request_ = request;
  frame_request_ = false;
  const Token token{next_token_++};
  active_token_ = token;
  claimed_ = false;
  ready_.reset();
  ready_frame_.reset();
  return token;
}

DrawCaptureMailbox::Token DrawCaptureMailbox::StartFrame(const std::filesystem::path& output_path) {
  std::lock_guard lock(mutex_);
  if (active_token_ || !next_token_) {
    return {};
  }
  request_ = {};
  request_.output_path = output_path;
  frame_request_ = true;
  const Token token{next_token_++};
  active_token_ = token;
  claimed_ = false;
  ready_.reset();
  ready_frame_.reset();
  return token;
}

bool DrawCaptureMailbox::IsPending(Token token) const {
  std::lock_guard lock(mutex_);
  return token && token == active_token_;
}

std::shared_ptr<const DrawCaptureSnapshot> DrawCaptureMailbox::TryTake(Token token) {
  std::lock_guard lock(mutex_);
  if (!token || token != active_token_ || !ready_) {
    return {};
  }
  std::shared_ptr<const DrawCaptureSnapshot> snapshot = std::move(ready_);
  ResetLocked();
  return snapshot;
}

std::shared_ptr<const DrawCaptureFrameSnapshot> DrawCaptureMailbox::TryTakeFrame(Token token) {
  std::lock_guard lock(mutex_);
  if (!token || token != active_token_ || !frame_request_ || !ready_frame_) {
    return {};
  }
  std::shared_ptr<const DrawCaptureFrameSnapshot> snapshot = std::move(ready_frame_);
  ResetLocked();
  return snapshot;
}

bool DrawCaptureMailbox::Cancel(Token token) {
  std::lock_guard lock(mutex_);
  if (!token || token != active_token_) {
    return false;
  }
  ResetLocked();
  return true;
}

DrawCaptureMailbox::Token DrawCaptureMailbox::GetRequestToken(
    const std::filesystem::path& output_path) const {
  std::lock_guard lock(mutex_);
  return !frame_request_ && output_path == request_.output_path ? active_token_ : Token{};
}

DrawCaptureMailbox::Token DrawCaptureMailbox::Claim(Token expected_token,
                                                    const std::filesystem::path& output_path,
                                                    uint64_t vertex_shader_hash,
                                                    uint64_t pixel_shader_hash) {
  std::lock_guard lock(mutex_);
  if (!expected_token || expected_token != active_token_ || frame_request_ || claimed_ ||
      output_path != request_.output_path || vertex_shader_hash != request_.vertex_shader_hash ||
      pixel_shader_hash != request_.pixel_shader_hash) {
    return {};
  }
  claimed_ = true;
  return active_token_;
}

bool DrawCaptureMailbox::Publish(Token token, std::shared_ptr<const DrawCaptureSnapshot> snapshot) {
  std::lock_guard lock(mutex_);
  if (!token || token != active_token_ || frame_request_ || !claimed_ || !snapshot || ready_ ||
      ready_frame_ || snapshot->vertex_shader_hash != request_.vertex_shader_hash ||
      snapshot->pixel_shader_hash != request_.pixel_shader_hash) {
    return false;
  }
  ready_ = std::move(snapshot);
  return true;
}

DrawCaptureMailbox::Token DrawCaptureMailbox::ClaimFrame(Token expected_token,
                                                         const std::filesystem::path& output_path) {
  std::lock_guard lock(mutex_);
  if (!expected_token || expected_token != active_token_ || !frame_request_ || claimed_ ||
      output_path != request_.output_path) {
    return {};
  }
  claimed_ = true;
  return active_token_;
}

bool DrawCaptureMailbox::IsFrameRequest(Token token) const {
  std::lock_guard lock(mutex_);
  return token && token == active_token_ && frame_request_;
}

DrawCaptureMailbox::Token DrawCaptureMailbox::GetFrameRequestToken() const {
  std::lock_guard lock(mutex_);
  // A claimed request belongs to its producer until the consumer takes it.
  // Exposing it again after publication would let the next swap discard it.
  return frame_request_ && !claimed_ ? active_token_ : Token{};
}

std::filesystem::path DrawCaptureMailbox::GetRequestPath(Token token) const {
  std::lock_guard lock(mutex_);
  return token && token == active_token_ ? request_.output_path : std::filesystem::path();
}

bool DrawCaptureMailbox::PublishFrame(Token token,
                                      std::shared_ptr<const DrawCaptureFrameSnapshot> snapshot) {
  std::lock_guard lock(mutex_);
  if (!token || token != active_token_ || !frame_request_ || !claimed_ || !snapshot || ready_ ||
      ready_frame_) {
    return false;
  }
  ready_frame_ = std::move(snapshot);
  return true;
}

bool DrawCaptureMailbox::Fail(Token token) {
  std::lock_guard lock(mutex_);
  if (!token || token != active_token_) {
    return false;
  }
  ResetLocked();
  return true;
}

DrawCaptureMailbox::ObserverToken DrawCaptureMailbox::RegisterFrameInputObserver(
    std::shared_ptr<DrawCaptureFrameInputObserver> observer) {
  if (!observer) {
    return {};
  }
  const bool wants_prepared_draws = observer->WantsPreparedDraws();
  const bool wants_prepared_copies = observer->WantsPreparedCopies();
  const bool wants_prepared_swaps = observer->WantsPreparedSwaps();
  std::lock_guard lock(observer_mutex_);
  if (observer_state_ && (observer_state_->active || observer_state_->in_flight)) {
    return {};
  }
  if (!next_observer_token_) {
    return {};
  }
  auto state = std::make_shared<ObserverState>();
  state->token = ObserverToken{next_observer_token_++};
  state->observer = std::move(observer);
  observer_state_ = state;
  observer_prepared_draw_token_.store(wants_prepared_draws ? state->token.value : 0,
                                      std::memory_order_release);
  observer_prepared_copy_token_.store(wants_prepared_copies ? state->token.value : 0,
                                      std::memory_order_release);
  observer_prepared_swap_token_.store(wants_prepared_swaps ? state->token.value : 0,
                                      std::memory_order_release);
  observer_fast_token_.store(state->token.value, std::memory_order_release);
  return state->token;
}

bool DrawCaptureMailbox::UnregisterFrameInputObserver(ObserverToken token) {
  if (!token) {
    return false;
  }
  std::unique_lock lock(observer_mutex_);
  const std::shared_ptr<ObserverState> state = observer_state_;
  if (!state || state->token != token) {
    return false;
  }
  state->active = false;
  state->unregistering = true;
  observer_fast_token_.store(0, std::memory_order_release);
  observer_prepared_draw_token_.store(0, std::memory_order_release);
  observer_prepared_copy_token_.store(0, std::memory_order_release);
  observer_prepared_swap_token_.store(0, std::memory_order_release);
  if (callback_state == state.get()) {
    // The caller is inside this observer. Waiting here would deadlock; the
    // dispatch epilogue will complete the requested unregister.
    return false;
  }
  state->quiesced.wait(lock, [&state] { return state->in_flight == 0; });
  if (observer_state_ == state) {
    observer_state_.reset();
  }
  return true;
}

bool DrawCaptureMailbox::HasFrameInputObserver() const noexcept {
  return observer_fast_token_.load(std::memory_order_acquire) != 0;
}

DrawCaptureMailbox::ObserverToken DrawCaptureMailbox::GetFrameInputObserverToken() const noexcept {
  return ObserverToken{observer_fast_token_.load(std::memory_order_acquire)};
}

bool DrawCaptureMailbox::HasFrameInputPreparedDrawObserver(ObserverToken token) const noexcept {
  return token && observer_prepared_draw_token_.load(std::memory_order_acquire) == token.value;
}

bool DrawCaptureMailbox::HasFrameInputPreparedCopyObserver(ObserverToken token) const noexcept {
  return token && observer_prepared_copy_token_.load(std::memory_order_acquire) == token.value;
}

bool DrawCaptureMailbox::HasFrameInputPreparedSwapObserver(ObserverToken token) const noexcept {
  return token && observer_prepared_swap_token_.load(std::memory_order_acquire) == token.value;
}

template <typename Callback>
bool DrawCaptureMailbox::DispatchFrameInputObserver(ObserverToken token, Callback&& callback) {
  if (!token || observer_fast_token_.load(std::memory_order_acquire) != token.value) {
    return false;
  }

  std::shared_ptr<ObserverState> state;
  {
    std::lock_guard lock(observer_mutex_);
    state = observer_state_;
    if (!state || state->token != token || !state->active || state->in_flight != 0) {
      return false;
    }
    ++state->in_flight;
  }

  bool result = false;
  void* previous_callback_state = callback_state;
  callback_state = state.get();
  try {
    result = callback(*state->observer);
  } catch (...) {
    // Observer exceptions must not escape into PM4 execution or change the
    // guest command's result.
    result = false;
  }
  callback_state = previous_callback_state;

  {
    std::lock_guard lock(observer_mutex_);
    if (!result) {
      state->active = false;
      observer_fast_token_.store(0, std::memory_order_release);
      observer_prepared_draw_token_.store(0, std::memory_order_release);
      observer_prepared_copy_token_.store(0, std::memory_order_release);
      observer_prepared_swap_token_.store(0, std::memory_order_release);
    }
    assert(state->in_flight != 0);
    --state->in_flight;
    if (!state->in_flight) {
      state->quiesced.notify_all();
      if (state->unregistering && observer_state_ == state) {
        observer_state_.reset();
      }
    }
  }
  return result;
}

bool DrawCaptureMailbox::DispatchFrameInputBegin(ObserverToken token,
                                                 const DrawCaptureFrameInputSwapView& boundary) {
  return DispatchFrameInputObserver(token, [&boundary](DrawCaptureFrameInputObserver& observer) {
    return observer.OnFrameBegin(boundary);
  });
}

bool DrawCaptureMailbox::DispatchFrameInputDraw(ObserverToken token,
                                                const DrawCaptureFrameInputDrawView& draw) {
  return DispatchFrameInputObserver(
      token, [&draw](DrawCaptureFrameInputObserver& observer) { return observer.OnDraw(draw); });
}

bool DrawCaptureMailbox::DispatchFrameInputPreparedDraw(ObserverToken token,
                                                        const DrawCaptureFrameInputDrawView& draw) {
  if (!HasFrameInputPreparedDrawObserver(token) || draw.original_host_issued ||
      draw.execution_status != DrawCaptureExecutionStatus::kUnfinished ||
      draw.execution_reason != DrawCaptureReason::kNone) {
    return false;
  }
  return DispatchFrameInputObserver(token, [&draw](DrawCaptureFrameInputObserver& observer) {
    return observer.OnDrawPrepared(draw);
  });
}

bool DrawCaptureMailbox::DispatchFrameInputCopy(ObserverToken token,
                                                const DrawCaptureFrameInputCopyView& copy) {
  return DispatchFrameInputObserver(
      token, [&copy](DrawCaptureFrameInputObserver& observer) { return observer.OnCopy(copy); });
}

bool DrawCaptureMailbox::DispatchFrameInputPreparedCopy(ObserverToken token,
                                                        const DrawCaptureFrameInputCopyView& copy) {
  if (!HasFrameInputPreparedCopyObserver(token) || copy.original_succeeded ||
      copy.written_address || copy.written_length) {
    return false;
  }
  return DispatchFrameInputObserver(token, [&copy](DrawCaptureFrameInputObserver& observer) {
    return observer.OnCopyPrepared(copy);
  });
}

bool DrawCaptureMailbox::DispatchFrameInputSwap(ObserverToken token,
                                                const DrawCaptureFrameInputSwapView& swap) {
  return DispatchFrameInputObserver(
      token, [&swap](DrawCaptureFrameInputObserver& observer) { return observer.OnSwap(swap); });
}

bool DrawCaptureMailbox::DispatchFrameInputPreparedSwap(ObserverToken token,
                                                        const DrawCaptureFrameInputSwapView& swap) {
  if (!HasFrameInputPreparedSwapObserver(token) || swap.original_succeeded) {
    return false;
  }
  return DispatchFrameInputObserver(token, [&swap](DrawCaptureFrameInputObserver& observer) {
    return observer.OnSwapPrepared(swap);
  });
}

bool DrawCaptureMailbox::DispatchFrameInputEnd(ObserverToken token, uint64_t frame,
                                               uint64_t submission, bool original_succeeded) {
  return DispatchFrameInputObserver(
      token, [frame, submission, original_succeeded](DrawCaptureFrameInputObserver& observer) {
        return observer.OnFrameEnd(frame, submission, original_succeeded);
      });
}

bool DrawCaptureMailbox::HasActiveRequest() const {
  std::lock_guard lock(mutex_);
  return bool(active_token_);
}

void DrawCaptureMailbox::FailCurrent() {
  std::lock_guard lock(mutex_);
  ResetLocked();
}

}  // namespace rex::graphics::diagnostic
