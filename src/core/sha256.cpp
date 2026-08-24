#include <rex/crypto/sha256.h>

#include <array>
#include <fstream>

#include "thirdparty/crypto/sha256.h"

namespace rex::crypto {

std::string sha256(std::string_view data) {
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
