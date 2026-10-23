#include "relay_invite.h"

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cstdio>
#include <optional>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "thirdparty/crypto/sha256.h"

namespace rex::relay {
namespace {
void Wipe(std::span<uint8_t> bytes) {
  volatile uint8_t* data = bytes.data();
  for (size_t i = 0; i < bytes.size(); ++i)
    data[i] = 0;
}
template <size_t N>
struct Buffer {
  std::array<uint8_t, N> bytes{};
  ~Buffer() { Wipe(bytes); }
};
bool ValidHost(std::string_view host) {
  return !host.empty() && host.size() <= 253 &&
         std::all_of(host.begin(), host.end(), [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '.';
         });
}
Error Damaged() {
  return {ErrorCategory::Config, "invite code is damaged or incomplete"};
}
char Upper(char c) {
  return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c;
}
Digest Hash(std::span<const uint8_t> bytes) {
  ::sha256::SHA256 hash;
  hash.add(bytes.data(), bytes.size());
  Digest digest{};
  hash.getHash(digest.data());
  return digest;
}
}  // namespace

Result<InviteAddress> ParseInviteAddress(std::string_view address) {
  const auto colon = address.find(':');
  if (colon == std::string_view::npos || !ValidHost(address.substr(0, colon)))
    return std::unexpected(
        Error{ErrorCategory::Config, "Relay address requires an IPv4 or DNS host:port"});
  const auto number = address.substr(colon + 1);
  unsigned port = 0;
  const auto parsed = std::from_chars(number.data(), number.data() + number.size(), port);
  if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || !port ||
      port > 65535)
    return std::unexpected(Error{ErrorCategory::Config, "Relay port must be 1-65535"});
  return InviteAddress{std::string(address.substr(0, colon)), static_cast<uint16_t>(port)};
}

Invite::Invite(Invite&& other) noexcept
    : address(std::move(other.address)),
      room(std::move(other.room)),
      secret(other.secret),
      secret_size(other.secret_size) {
  Wipe(other.secret);
  other.secret_size = 0;
}
Invite::~Invite() {
  Wipe(secret);
}
std::string_view Invite::SecretView() const {
  return {reinterpret_cast<const char*>(secret.data()), secret_size};
}

Result<std::string> EncodeInvite(const InviteAddress& address, std::string_view room,
                                 std::string_view secret) {
  if (!ValidHost(address.host) || !address.port || room.empty() || room.size() > 32 ||
      secret.size() < 16 || secret.size() > 64)
    return std::unexpected(Damaged());
  Buffer<359> payload;
  size_t size = 0;
  auto put = [&](uint8_t byte) { payload.bytes[size++] = byte; };
  auto field = [&](std::string_view value) {
    put(static_cast<uint8_t>(value.size()));
    for (unsigned char byte : value)
      put(byte);
  };
  put(1);
  put(static_cast<uint8_t>(address.port));
  put(static_cast<uint8_t>(address.port >> 8));
  field(address.host);
  field(room);
  field(secret);
  const auto checksum = Hash({payload.bytes.data(), size});
  for (size_t i = 0; i < 4; ++i)
    put(checksum[i]);
  std::string code = "RXR1-";
  uint32_t bits = 0;
  unsigned count = 0;
  for (size_t i = 0; i < size; ++i) {
    bits = (bits << 8) | payload.bytes[i];
    count += 8;
    while (count >= 5) {
      count -= 5;
      code += kInviteAlphabet[(bits >> count) & 31];
    }
  }
  if (count)
    code += kInviteAlphabet[(bits << (5 - count)) & 31];
  return code;
}

Result<Invite> DecodeInvite(std::string_view code) {
  constexpr std::string_view prefix = "RXR1-";
  if (code.size() < prefix.size() || !std::equal(prefix.begin(), prefix.end(), code.begin(),
                                                 [](char a, char b) { return a == Upper(b); }))
    return std::unexpected(Error{ErrorCategory::Config, "not an invite code"});
  Buffer<359> payload;
  size_t size = 0;
  uint32_t bits = 0;
  unsigned count = 0;
  for (char c : code.substr(prefix.size())) {
    if (c == ' ' || c == '-')
      continue;
    c = Upper(c);
    if (c == 'O')
      c = '0';
    if (c == 'I' || c == 'L')
      c = '1';
    const auto value = kInviteAlphabet.find(c);
    if (value == std::string_view::npos)
      return std::unexpected(Damaged());
    bits = (bits << 5) | static_cast<uint32_t>(value);
    count += 5;
    if (count >= 8) {
      count -= 8;
      if (size == payload.bytes.size())
        return std::unexpected(Damaged());
      payload.bytes[size++] = static_cast<uint8_t>(bits >> count);
    }
  }
  if (count >= 5 || (bits & ((1u << count) - 1)) || size < 10)
    return std::unexpected(Damaged());
  const auto checksum = Hash({payload.bytes.data(), size - 4});
  if (!std::equal(checksum.begin(), checksum.begin() + 4, payload.bytes.begin() + size - 4))
    return std::unexpected(Damaged());
  if (payload.bytes[0] > 1)
    return std::unexpected(
        Error{ErrorCategory::Config, "invite code was made for a newer version"});
  if (payload.bytes[0] != 1)
    return std::unexpected(Damaged());
  Invite invite;
  invite.address.port = uint16_t(payload.bytes[1]) | (uint16_t(payload.bytes[2]) << 8);
  size_t offset = 3;
  auto field = [&]() -> std::optional<std::string_view> {
    if (offset >= size - 4)
      return {};
    size_t length = payload.bytes[offset++];
    if (length > size - 4 - offset)
      return {};
    std::string_view value(reinterpret_cast<const char*>(payload.bytes.data() + offset), length);
    offset += length;
    return value;
  };
  auto host = field();
  auto room = field();
  auto secret = field();
  if (!host || !room || !secret || !ValidHost(*host) || !invite.address.port || room->empty() ||
      room->size() > 32 || secret->size() < 16 || secret->size() > 64 || offset != size - 4)
    return std::unexpected(Damaged());
  invite.address.host = *host;
  invite.room = *room;
  invite.secret_size = secret->size();
  std::copy(secret->begin(), secret->end(), invite.secret.begin());
  return invite;
}

Result<size_t> ReadSecretFile(const std::filesystem::path& path, std::span<char> storage) {
#if defined(_WIN32)
  const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return std::unexpected(Error{ErrorCategory::IO, "Cannot read relay secret file"});
  BY_HANDLE_FILE_INFORMATION info{};
  bool failed = GetFileType(file) != FILE_TYPE_DISK || !GetFileInformationByHandle(file, &info) ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
#else
  const auto file = open(path.c_str(), O_RDONLY | O_NONBLOCK);
  if (file < 0)
    return std::unexpected(Error{ErrorCategory::IO, "Cannot read relay secret file"});
  struct stat info{};
  bool failed = fstat(file, &info) != 0 || !S_ISREG(info.st_mode);
#endif
  size_t size = 0;
  while (!failed && size < storage.size()) {
#if defined(_WIN32)
    DWORD count = 0;
    if (!ReadFile(file, storage.data() + size, static_cast<DWORD>(storage.size() - size), &count,
                  nullptr)) {
#else
    const auto count = read(file, storage.data() + size, storage.size() - size);
    if (count < 0) {
      if (errno == EINTR)
        continue;
#endif
      failed = true;
      break;
    }
    if (!count)
      break;
    size += static_cast<size_t>(count);
  }
#if defined(_WIN32)
  CloseHandle(file);
#else
  close(file);
#endif
  if (failed || size == storage.size())
    return std::unexpected(Error{
        ErrorCategory::IO, "Relay secret must be a readable regular file within the size limit"});
  return size;
}

Result<std::string> MakeInvite(std::string_view address, const std::filesystem::path& secret_file,
                               std::string_view room, bool create_secret,
                               Result<Digest> (*random_source)()) {
  auto endpoint = ParseInviteAddress(address);
  if (!endpoint)
    return std::unexpected(endpoint.error());
  if (room.empty() || room.size() > 32)
    return std::unexpected(Error{ErrorCategory::Config, "Relay room must be 1-32 bytes"});
  if (create_secret) {
    // Exclusive creation prevents races and preserves every existing file.
#if defined(_WIN32)
    const auto file = CreateFileW(secret_file.c_str(), GENERIC_WRITE | DELETE, 0, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool opened = file != INVALID_HANDLE_VALUE;
    const bool exists =
        !opened && (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS);
#else
    const auto file = open(secret_file.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    const bool opened = file >= 0;
    const bool exists = !opened && errno == EEXIST;
#endif
    if (!opened && !exists)
      return std::unexpected(Error{ErrorCategory::IO, "Cannot create relay secret file"});
    if (opened) {
      auto random = random_source();
      Buffer<33> text;
      if (random) {
        for (size_t i = 0; i < 32; ++i)
          text.bytes[i] = kInviteAlphabet[(*random)[i] & 31];
        Wipe(*random);
      }
      text.bytes[32] = '\n';
#if defined(_WIN32)
      DWORD written = 0;
      bool ok =
          random && WriteFile(file, text.bytes.data(), 33, &written, nullptr) && written == 33;
      ok = ok && FlushFileBuffers(file);
      if (!ok) {
        // Delete by handle while exclusive ownership still identifies our file.
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(file, FileDispositionInfo, &disposition,
                                        sizeof(disposition))) {
          CloseHandle(file);
          return std::unexpected(
              Error{ErrorCategory::IO, "Cannot remove partial relay secret file"});
        }
      }
      ok = CloseHandle(file) && ok;
#else
      bool ok = random && write(file, text.bytes.data(), 33) == 33 && fsync(file) == 0;
      if (!ok) {
        struct stat opened_info{}, path_info{};
        if (fstat(file, &opened_info) || lstat(secret_file.c_str(), &path_info) ||
            opened_info.st_dev != path_info.st_dev || opened_info.st_ino != path_info.st_ino ||
            unlink(secret_file.c_str())) {
          close(file);
          return std::unexpected(
              Error{ErrorCategory::IO, "Cannot remove partial relay secret file"});
        }
      }
      ok = (close(file) == 0) && ok;
#endif
      if (!random)
        return std::unexpected(random.error());
      if (!ok)
        return std::unexpected(Error{ErrorCategory::IO, "Cannot write relay secret file"});
    }
  }
  // Match the relay command's 4 KiB bound and ASCII whitespace trimming.
  Buffer<4097> bytes;
  auto size = ReadSecretFile(secret_file,
                             {reinterpret_cast<char*>(bytes.bytes.data()), bytes.bytes.size()});
  if (!size)
    return std::unexpected(size.error());
  std::string_view secret(reinterpret_cast<const char*>(bytes.bytes.data()), *size);
  const auto first = secret.find_first_not_of(" \t\r\n\f\v");
  if (first == std::string_view::npos)
    secret = {};
  else
    secret = secret.substr(first, secret.find_last_not_of(" \t\r\n\f\v") - first + 1);
  if (secret.size() < 16 || secret.size() > 64)
    return std::unexpected(
        Error{ErrorCategory::Config, "Invite secret must be 16-64 bytes after trimming"});
  return EncodeInvite(*endpoint, room, secret);
}
}  // namespace rex::relay
