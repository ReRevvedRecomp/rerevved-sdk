#pragma once

#include "relay_protocol.h"

#include <filesystem>

namespace rex::relay {

inline constexpr std::string_view kInviteAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

struct InviteAddress {
  std::string host;
  uint16_t port = 0;
};
Result<InviteAddress> ParseInviteAddress(std::string_view address);

// Decoded credentials have bounded storage that is wiped, including moved-from copies.
struct Invite {
  Invite() = default;
  Invite(Invite&& other) noexcept;
  Invite(const Invite&) = delete;
  ~Invite();
  InviteAddress address;
  std::string room;
  std::array<uint8_t, 64> secret{};
  size_t secret_size = 0;
  std::string_view SecretView() const;
};

Result<std::string> EncodeInvite(const InviteAddress& address, std::string_view room,
                                 std::string_view secret);
Result<Invite> DecodeInvite(std::string_view code);

// Read a regular file directly into caller-owned wiped storage. The last byte
// detects overflow, so storage must be one byte larger than the accepted limit.
Result<size_t> ReadSecretFile(const std::filesystem::path& path, std::span<char> storage);

// Creates an invite without resolving its host.
Result<std::string> MakeInvite(std::string_view address, const std::filesystem::path& secret_file,
                               std::string_view room = "testers", bool create_secret = false,
                               Result<Digest> (*random_source)() = RandomSeed);

}  // namespace rex::relay
