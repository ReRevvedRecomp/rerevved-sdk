#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/draw_capture_mailbox.h>

namespace rex::graphics::diagnostic {
namespace {

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

}  // namespace
}  // namespace rex::graphics::diagnostic
