/**
 * @file        core/system_posix.cpp
 * @brief       POSIX implementations of rex/system.h platform helpers.
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <cstdio>
#include <string>
#include <system_error>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_misc.h>

#include <rex/logging.h>
#include <rex/system.h>

namespace rex {

void LaunchWebBrowser(const std::string_view url) {
  const std::string text(url);
  if (!SDL_OpenURL(text.c_str())) {
    REXLOG_WARN("Could not open {}: {}", text, SDL_GetError());
  }
}

void LaunchFileExplorer(const std::filesystem::path& path) {
  // Absolute first, so a relative file name still has a parent folder.
  std::error_code ec;
  auto target = std::filesystem::absolute(path, ec);
  if (ec) {
    target = path;
  }
  // A file opens its containing folder rather than the file itself.
  const auto folder = std::filesystem::is_regular_file(target, ec) ? target.parent_path() : target;
  // Percent-encode everything outside the RFC 3986 unreserved set and '/'.
  std::string url = "file://";
  for (const unsigned char c : folder.string()) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '.' || c == '_' || c == '~' || c == '/') {
      url += static_cast<char>(c);
    } else {
      static constexpr char kHex[] = "0123456789ABCDEF";
      url += '%';
      url += kHex[c >> 4];
      url += kHex[c & 0xF];
    }
  }
  LaunchWebBrowser(url);
}

// TODO(tomc): add linux support for showing a native message box
void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  const char* level = "INFO";
  switch (type) {
    case SimpleMessageBoxType::Help:
      level = "INFO";
      break;
    case SimpleMessageBoxType::Warning:
      level = "WARNING";
      break;
    case SimpleMessageBoxType::Error:
      level = "ERROR";
      break;
  }
  std::fprintf(stderr, "[%s] %.*s\n", level, static_cast<int>(message.size()), message.data());
  std::fflush(stderr);
}

}  // namespace rex
