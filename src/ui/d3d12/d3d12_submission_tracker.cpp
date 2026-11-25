/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_submission_tracker.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace rex::ui::d3d12 {

namespace queue_worker {
namespace {
struct State {
  std::mutex mutex;
  std::condition_variable work_cv;
  std::condition_variable done_cv;
  std::deque<std::pair<uint64_t, Operation>> operations;
  std::thread thread;
  ID3D12CommandQueue* queue = nullptr;
  bool accepting = false;
  bool stop = false;
  uint64_t enqueued = 0;
  uint64_t performed = 0;
};
// Never destroyed: the thread must not be joined during DLL unload.
State& GetState() {
  static State* state = new State;
  return *state;
}

void ThreadMain(ID3D12CommandQueue* queue) {
  SetThreadDescription(GetCurrentThread(), L"GPU Queue Submission");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  State& state = GetState();
  std::unique_lock<std::mutex> lock(state.mutex);
  for (;;) {
    state.work_cv.wait(lock, [&state]() { return state.stop || !state.operations.empty(); });
    if (state.operations.empty()) {
      break;
    }
    std::pair<uint64_t, Operation> operation = std::move(state.operations.front());
    state.operations.pop_front();
    lock.unlock();
    operation.second(queue);
    // Release whatever the operation holds before reporting it as performed.
    operation.second = nullptr;
    lock.lock();
    state.performed = operation.first;
    state.done_cv.notify_all();
  }
}
}  // namespace

bool Start(ID3D12CommandQueue* queue) {
  Stop();
  State& state = GetState();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.queue = queue;
  state.stop = false;
  try {
    state.thread = std::thread(ThreadMain, queue);
  } catch (...) {
    state.queue = nullptr;
    return false;
  }
  state.accepting = true;
  return true;
}

void Stop() {
  State& state = GetState();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.thread.joinable()) {
      return;
    }
    state.accepting = false;
    state.stop = true;
  }
  state.work_cv.notify_all();
  state.thread.join();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.queue = nullptr;
  state.done_cv.notify_all();
}

bool IsActive(ID3D12CommandQueue* queue) {
  State& state = GetState();
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.accepting && queue && state.queue == queue;
}

uint64_t Run(ID3D12CommandQueue* queue, Operation operation) {
  State& state = GetState();
  {
    std::unique_lock<std::mutex> lock(state.mutex);
    if (state.accepting && queue && state.queue == queue) {
      uint64_t sequence = ++state.enqueued;
      state.operations.emplace_back(sequence, std::move(operation));
      lock.unlock();
      state.work_cv.notify_one();
      return sequence;
    }
    state.done_cv.wait(lock, [&state]() { return state.performed >= state.enqueued; });
  }
  operation(queue);
  return 0;
}

void WaitFor(uint64_t sequence) {
  if (!sequence) {
    return;
  }
  State& state = GetState();
  std::unique_lock<std::mutex> lock(state.mutex);
  state.done_cv.wait(
      lock, [&state, sequence]() { return state.performed >= std::min(sequence, state.enqueued); });
}

void Drain(ID3D12CommandQueue* queue) {
  State& state = GetState();
  std::unique_lock<std::mutex> lock(state.mutex);
  if (!queue || state.queue != queue) {
    return;
  }
  state.done_cv.wait(lock, [&state]() { return state.performed >= state.enqueued; });
}
}  // namespace queue_worker

bool D3D12SubmissionTracker::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue) {
  Shutdown();
  fence_completion_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (!fence_completion_event_) {
    REXLOG_ERROR("D3D12SubmissionTracker: Failed to create the fence completion event");
    Shutdown();
    return false;
  }
  // Continue where the tracker was left at the last shutdown.
  if (FAILED(device->CreateFence(submission_current_ - 1, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(&fence_)))) {
    REXLOG_ERROR("D3D12SubmissionTracker: Failed to create the fence");
    Shutdown();
    return false;
  }
  queue_ = queue;
  submission_signal_queued_ = submission_current_ - 1;
  return true;
}

void D3D12SubmissionTracker::Shutdown() {
  AwaitAllSubmissionsCompletion();
  queue_.Reset();
  fence_.Reset();
  if (fence_completion_event_) {
    CloseHandle(fence_completion_event_);
    fence_completion_event_ = nullptr;
  }
}

bool D3D12SubmissionTracker::AwaitSubmissionCompletion(UINT64 submission_index) {
  if (!fence_ || !fence_completion_event_) {
    // Not fully initialized yet or already shut down.
    return false;
  }
  // The tracker itself can't give a submission index for a submission that
  // hasn't even started being recorded yet, the client has provided a
  // completely invalid value or has done overly optimistic math if such an
  // index has been obtained somehow.
  assert_true(submission_index <= submission_current_);
  // Waiting for the current submission is fine if there was a refusal to
  // submit, and the submission index wasn't incremented, but still need to
  // release objects referenced in the dropped submission (while shutting down,
  // for instance - in this case, waiting for the last successful submission,
  // which could have also referenced the objects from the new submission - we
  // can't know since the client has already overwritten its last usage index,
  // would correctly ensure that GPU usage of the objects is not pending).
  // Waiting for successful submissions, but failed signals, will result in a
  // true race condition, however, but waiting for the closest successful signal
  // is the best approximation - also retrying to signal in this case.
  UINT64 fence_value = submission_index;
  if (submission_index > submission_signal_queued_) {
    TrySignalEnqueueing();
    fence_value = submission_signal_queued_;
  }
  if (fence_->GetCompletedValue() < fence_value) {
    if (FAILED(fence_->SetEventOnCompletion(fence_value, fence_completion_event_))) {
      return false;
    }
    // A removed device may leave the fence unsignaled, so poll for removal.
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    (void)fence_->GetDevice(IID_PPV_ARGS(&device));
    for (;;) {
      const DWORD wait_result = WaitForSingleObject(fence_completion_event_, 250);
      if (wait_result == WAIT_OBJECT_0) {
        break;
      }
      if (wait_result == WAIT_TIMEOUT && signal_failed_->load()) {
        return false;
      }
      if (wait_result != WAIT_TIMEOUT || (device && FAILED(device->GetDeviceRemovedReason()))) {
        return false;
      }
    }
  }
  return fence_value == submission_index;
}

void D3D12SubmissionTracker::SetQueue(ID3D12CommandQueue* new_queue) {
  if (queue_.Get() == new_queue) {
    return;
  }
  if (queue_) {
    // Make sure the first signal on the new queue won't happen before the last
    // signal, if pending, on the old one, as that would result first in too
    // early submission completion indication, and then in rewinding.
    AwaitAllSubmissionsCompletion();
  }
  queue_ = new_queue;
}

bool D3D12SubmissionTracker::NextSubmission() {
  ++submission_current_;
  assert_not_null(queue_);
  assert_not_null(fence_);
  return TrySignalEnqueueing();
}

bool D3D12SubmissionTracker::TrySignalEnqueueing() {
  if (submission_signal_queued_ + 1 >= submission_current_) {
    return true;
  }
  if (!queue_ || !fence_) {
    return false;
  }
  UINT64 signal_value = submission_current_ - 1;
  if (queue_worker::IsActive(queue_.Get())) {
    Microsoft::WRL::ComPtr<ID3D12Fence> fence = fence_;
    std::shared_ptr<std::atomic<bool>> signal_failed = signal_failed_;
    queue_worker::Run(queue_.Get(),
                      [fence, signal_value, signal_failed](ID3D12CommandQueue* queue) {
                        if (FAILED(queue->Signal(fence.Get(), signal_value))) {
                          signal_failed->store(true);
                          REXLOG_ERROR(
                              "D3D12SubmissionTracker: Failed to signal the fence on the queue "
                              "worker");
                        }
                      });
  } else {
    // A worker that is stopping may still hold operations the signal must follow.
    queue_worker::Drain(queue_.Get());
    if (FAILED(queue_->Signal(fence_.Get(), signal_value))) {
      return false;
    }
  }
  submission_signal_queued_ = signal_value;
  return true;
}

}  // namespace rex::ui::d3d12
