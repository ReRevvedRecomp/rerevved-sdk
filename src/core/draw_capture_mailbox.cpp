#include <rex/graphics/draw_capture_mailbox.h>

#include <utility>

namespace rex::graphics::diagnostic {

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

bool DrawCaptureMailbox::HasActiveRequest() const {
  std::lock_guard lock(mutex_);
  return bool(active_token_);
}

void DrawCaptureMailbox::FailCurrent() {
  std::lock_guard lock(mutex_);
  ResetLocked();
}

}  // namespace rex::graphics::diagnostic
