#include <catch2/catch_test_macros.hpp>

#include "core/log_redact.h"

using rex::detail::RedactFolder;

TEST_CASE("RedactFolder replaces the folder in any case and separator", "[core][logging]") {
  CHECK(RedactFolder(R"(Saved C:\Users\Ann\Games\x.toml)", R"(C:\Users\Ann)", "%USERPROFILE%") ==
        R"(Saved %USERPROFILE%\Games\x.toml)");
  CHECK(RedactFolder(R"(c:/users/ann/a and C:\USERS\ANN)", R"(C:\Users\Ann\)", "~") == "~/a and ~");
}

TEST_CASE("RedactFolder keeps longer names and unrelated text", "[core][logging]") {
  CHECK(RedactFolder(R"(C:\Users\Anna\x)", R"(C:\Users\Ann)", "~") == R"(C:\Users\Anna\x)");
  CHECK(RedactFolder("no paths here", R"(C:\Users\Ann)", "~") == "no paths here");
  CHECK(RedactFolder("short", "", "~") == "short");
}
