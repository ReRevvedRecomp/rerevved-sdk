#include <rex/crypto/sha256.h>

#include <array>
#include <fstream>
#include <limits>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#endif

#include "thirdparty/crypto/sha256.h"

namespace rex::crypto {

std::string sha256(std::string_view data) {
#if defined(_WIN32)
  // CNG dispatches to the platform's accelerated SHA implementation. Keep the
  // portable implementation for larger inputs and unavailable providers.
  if (data.size() <= std::numeric_limits<ULONG>::max()) {
    std::array<unsigned char, 32> bytes;
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                   reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                   static_cast<ULONG>(data.size()), bytes.data(),
                   static_cast<ULONG>(bytes.size())) >= 0) {
      constexpr char hex[] = "0123456789abcdef";
      std::string result(bytes.size() * 2, '0');
      for (size_t index = 0; index < bytes.size(); ++index) {
        result[index * 2] = hex[bytes[index] >> 4];
        result[index * 2 + 1] = hex[bytes[index] & 15];
      }
      return result;
    }
  }
#endif
  ::sha256::SHA256 digest;
  digest.add(data.data(), data.size());
  return digest.getHash();
}

std::string sha256_file(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }

  ::sha256::SHA256 digest;
  std::array<char, 256 * 1024> chunk;
  while (file) {
    file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const auto count = file.gcount();
    if (count > 0) {
      digest.add(chunk.data(), static_cast<size_t>(count));
    }
  }
  return file.eof() ? digest.getHash() : std::string{};
}

}  // namespace rex::crypto
