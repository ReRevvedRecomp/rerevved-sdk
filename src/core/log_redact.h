#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace rex::detail {

// Replaces each occurrence of folder with replacement. Matching ignores ASCII
// case and treats '/' and '\' alike, as Windows paths do. A match must end the
// path component, so "C:\Users\ann" leaves "C:\Users\anna" alone.
inline std::string RedactFolder(std::string_view text, std::string_view folder,
                                std::string_view replacement) {
  while (!folder.empty() && (folder.back() == '/' || folder.back() == '\\'))
    folder.remove_suffix(1);
  if (folder.empty() || text.size() < folder.size())
    return std::string(text);
  auto same = [](char a, char b) {
    const bool sa = a == '/' || a == '\\';
    const bool sb = b == '/' || b == '\\';
    if (sa || sb)
      return sa && sb;
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
  };
  auto ends_component = [](char c) {
    return !(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' ||
             static_cast<unsigned char>(c) >= 0x80);
  };
  std::string out;
  size_t copied = 0;
  for (size_t i = 0; i + folder.size() <= text.size();) {
    size_t n = 0;
    while (n < folder.size() && same(text[i + n], folder[n]))
      ++n;
    const size_t end = i + folder.size();
    if (n == folder.size() && (end == text.size() || ends_component(text[end]))) {
      out.append(text.substr(copied, i - copied));
      out.append(replacement);
      copied = end;
      i = end;
    } else {
      ++i;
    }
  }
  if (copied == 0)
    return std::string(text);
  out.append(text.substr(copied));
  return out;
}

}  // namespace rex::detail
