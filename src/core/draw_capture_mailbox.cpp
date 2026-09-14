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
  claimed_ = false;
  ready_.reset();
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
  const Token token{next_token_++};
  active_token_ = token;
  claimed_ = false;
  ready_.reset();
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
  return output_path == request_.output_path ? active_token_ : Token{};
}

DrawCaptureMailbox::Token DrawCaptureMailbox::Claim(Token expected_token,
                                                    const std::filesystem::path& output_path,
                                                    uint64_t vertex_shader_hash,
                                                    uint64_t pixel_shader_hash) {
  std::lock_guard lock(mutex_);
  if (!expected_token || expected_token != active_token_ || claimed_ ||
      output_path != request_.output_path || vertex_shader_hash != request_.vertex_shader_hash ||
      pixel_shader_hash != request_.pixel_shader_hash) {
    return {};
  }
  claimed_ = true;
  return active_token_;
}

bool DrawCaptureMailbox::Publish(Token token, std::shared_ptr<const DrawCaptureSnapshot> snapshot) {
  std::lock_guard lock(mutex_);
  if (!token || token != active_token_ || !claimed_ || !snapshot || ready_ ||
      snapshot->vertex_shader_hash != request_.vertex_shader_hash ||
      snapshot->pixel_shader_hash != request_.pixel_shader_hash) {
    return false;
  }
  ready_ = std::move(snapshot);
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
