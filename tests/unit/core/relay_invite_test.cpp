#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include "core/relay_invite.h"
#include "thirdparty/crypto/sha256.h"

using namespace rex::relay;
namespace {
constexpr std::string_view kSecret = "0123456789ABCDEF";
struct File {
  std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("invite-test-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::path path = directory / "fixture";
  File() { REQUIRE(std::filesystem::create_directory(directory)); }
  ~File() {
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(directory, error);
  }
};
std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), {}};
}
// Independent bit-by-bit encoding permits malformed payloads with valid checksums.
std::string RawCode(Bytes payload, bool checksum = true) {
  if (checksum) {
    ::sha256::SHA256 hash;
    hash.add(payload.data(), payload.size());
    Digest digest{};
    hash.getHash(digest.data());
    payload.insert(payload.end(), digest.begin(), digest.begin() + 4);
  }
  std::string result = "RXR1-";
  for (size_t i = 0; i < payload.size() * 8; i += 5) {
    unsigned value = 0;
    for (size_t bit = i; bit < i + 5; ++bit)
      value =
          (value << 1) | (bit < payload.size() * 8 ? (payload[bit / 8] >> (7 - bit % 8)) & 1 : 0);
    result += kInviteAlphabet[value];
  }
  return result;
}
Bytes Payload() {
  Bytes bytes{1, 0x94, 0x75, 1, 'a', 1, 'r', 16};
  bytes.insert(bytes.end(), kSecret.begin(), kSecret.end());
  return bytes;
}
void Damaged(std::string_view code) {
  auto result = DecodeInvite(code);
  REQUIRE_FALSE(result);
  CHECK(result.error().message == "invite code is damaged or incomplete");
}
}  // namespace

TEST_CASE("invite round trip accepts grouping lowercase and aliases", "[invite]") {
  auto code = EncodeInvite({"relay.example.net", 30100}, "testers", kSecret);
  REQUIRE(code);
  std::string decorated = "rxr1-";
  for (size_t i = 5; i < code->size(); ++i) {
    char c = (*code)[i];
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
    if (c == '0')
      c = 'o';
    if (c == '1')
      c = i % 2 ? 'i' : 'l';
    decorated += c;
    if (i % 4 == 0)
      decorated += " -";
  }
  auto decoded = DecodeInvite(decorated);
  REQUIRE(decoded);
  CHECK(decoded->address.host == "relay.example.net");
  CHECK(decoded->address.port == 30100);
  CHECK(decoded->room == "testers");
  CHECK(decoded->SecretView() == kSecret);
  auto maximum =
      EncodeInvite({std::string(253, 'a'), 65535}, std::string(32, '\0'), std::string(64, '\xff'));
  REQUIRE(maximum);
  REQUIRE(DecodeInvite(*maximum));
  CHECK(DecodeInvite(*maximum)->SecretView() == std::string(64, '\xff'));
}

TEST_CASE("invite fixed payload vector matches byte layout", "[invite]") {
  const auto raw = Payload();
  REQUIRE(raw.size() == 24);
  auto code = EncodeInvite({"a", 30100}, "r", kSecret);
  REQUIRE(code);
  CHECK(*code == RawCode(raw));
  auto decoded = DecodeInvite(RawCode(raw));
  REQUIRE(decoded);
  CHECK(decoded->address.port == 30100);
  CHECK(decoded->SecretView() == kSecret);
}

TEST_CASE("invite rejects prefix alphabet padding checksum and payload errors", "[invite]") {
  auto raw = Payload();
  auto code = RawCode(raw);
  for (std::string_view prefix : {"", "RXR2-", "RXR1", " RXR1-", "OTHER-"}) {
    auto result = DecodeInvite(std::string(prefix) + code.substr(5));
    REQUIRE_FALSE(result);
    CHECK(result.error().message == "not an invite code");
  }
  for (char invalid : {'U', '/', '=', '\t', '\n', char(0x80)}) {
    auto bad = code;
    bad[7] = invalid;
    Damaged(bad);
  }
  auto bad_checksum = code;
  bad_checksum[8] = bad_checksum[8] == '0' ? '2' : '0';
  Damaged(bad_checksum);
  auto trailing_bits = code;
  const auto last = kInviteAlphabet.find(trailing_bits.back());
  trailing_bits.back() = kInviteAlphabet[last | 1];
  Damaged(trailing_bits);
  Damaged(code + "0");
  Damaged(code.substr(0, code.size() - 2));
  Damaged("RXR1-");
  for (size_t length = 0; length < raw.size(); ++length)
    Damaged(RawCode(Bytes(raw.begin(), raw.begin() + length)));
  raw.push_back(0);
  Damaged(RawCode(raw));
  raw = Payload();
  raw[0] = 0;
  Damaged(RawCode(raw));
  raw[0] = 2;
  auto newer = DecodeInvite(RawCode(raw));
  REQUIRE_FALSE(newer);
  CHECK(newer.error().message == "invite code was made for a newer version");
  for (auto [index, value] :
       {std::pair{1, 0}, {3, 0}, {3, 254}, {4, '/'}, {5, 0}, {5, 33}, {7, 15}, {7, 65}}) {
    raw = Payload();
    raw[index] = static_cast<uint8_t>(value);
    if (index == 1)
      raw[2] = 0;
    Damaged(RawCode(raw));
  }
  Damaged(RawCode(Bytes(360, 0)));
}

TEST_CASE("invite validates endpoint and encoder field limits", "[invite]") {
  for (std::string_view address : {"", "host", ":1", "host:0", "host:65536", "host:-1", "host:2x",
                                   "[::1]:1", "a/b:1", "a b:1", "a:1:2", "a:"})
    CHECK_FALSE(ParseInviteAddress(address));
  CHECK_FALSE(ParseInviteAddress(std::string(254, 'a') + ":1"));
  CHECK(ParseInviteAddress("127.0.0.1:1"));
  CHECK(ParseInviteAddress("relay-1.example.net:65535"));
  CHECK_FALSE(EncodeInvite({"", 1}, "r", kSecret));
  CHECK_FALSE(EncodeInvite({"a", 0}, "r", kSecret));
  CHECK_FALSE(EncodeInvite({"a", 1}, "", kSecret));
  CHECK_FALSE(EncodeInvite({"a", 1}, std::string(33, 'r'), kSecret));
  CHECK_FALSE(EncodeInvite({"a", 1}, "r", std::string(15, 's')));
  CHECK_FALSE(EncodeInvite({"a", 1}, "r", std::string(65, 's')));
}

TEST_CASE("relay-invite creates reads and never overwrites secrets", "[invite][relay]") {
  File file;
  CHECK_FALSE(MakeInvite("host:1", file.path));
  auto made = MakeInvite("host:1", file.path, "testers", true);
  REQUIRE(made);
  const auto bytes = ReadFile(file.path);
  REQUIRE(bytes.size() == 33);
  CHECK(bytes.back() == '\n');
  CHECK(std::all_of(bytes.begin(), bytes.end() - 1,
                    [](char c) { return kInviteAlphabet.find(c) != std::string_view::npos; }));
  auto again = MakeInvite("host:1", file.path, "testers", true);
  REQUIRE(again);
  CHECK(*again == *made);
  CHECK(ReadFile(file.path) == bytes);
  REQUIRE(DecodeInvite(*made));
  CHECK(DecodeInvite(*made)->SecretView() == bytes.substr(0, 32));
  for (size_t size : {size_t{0}, size_t{15}, size_t{65}, size_t{4097}}) {
    {
      std::ofstream stream(file.path, std::ios::binary);
      stream << std::string(size, 'x');
    }
    CHECK_FALSE(MakeInvite("host:1", file.path));
  }
  {
    std::ofstream stream(file.path, std::ios::binary);
    stream << " \t\r\n" << kSecret << "\f\v ";
  }
  auto trimmed = MakeInvite("host:1", file.path);
  REQUIRE(trimmed);
  CHECK(DecodeInvite(*trimmed)->SecretView() == kSecret);
  CHECK_FALSE(MakeInvite("host:0", file.path));
  CHECK_FALSE(MakeInvite("host:1", file.path, ""));
}

TEST_CASE("relay-invite removes its partial file when randomness fails", "[invite][relay]") {
  File file;
  auto fail = []() -> rex::Result<Digest> {
    return std::unexpected(rex::Error{rex::ErrorCategory::IO, "injected random failure"});
  };
  CHECK_FALSE(MakeInvite("host:1", file.path, "testers", true, fail));
  CHECK_FALSE(std::filesystem::exists(file.path));
  {
    std::ofstream stream(file.path);
    stream << kSecret;
  }
  REQUIRE(MakeInvite("host:1", file.path, "testers", true, fail));
  CHECK(ReadFile(file.path) == kSecret);
}
