#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace rex::system {

/// Configuration for GameDataSelector, set by the guest application.
struct GameDataSelectorSettings {
  /// Whether startup targets XBLA. When false, the file dialog also offers an
  /// ISO filter; All files accepts STFS packages in either mode.
  bool is_xbla = false;
  /// Optional SHA-256 hex string. If non-empty, default.xex inside the game
  /// data *must* match this hash or the module fails with a message box.
  std::string default_xex_sha256;

  /// SHA-256 hex string for the title-update package file. If non-empty, a
  /// title update is required.
  std::string title_update_sha256;

  /// Optional SHA-256 for a raw default.xexp beside default.xex. The loader
  /// applies it directly, without an update: device or STFS package. Mutually
  /// exclusive with title_update_sha256.
  std::string sibling_xexp_sha256;

  /// Config file the resolved game_data_root is written back to, so the user
  /// is only asked once. Pass the app's config_path(). When empty, the path
  /// is derived from the executable name (`<exe stem>.toml` next to the
  /// executable).
  std::filesystem::path config_path;
};

/// Synchronous startup wizard that runs before any window or presenter is
/// created. Uses native SDL message boxes and file dialogs to
/// prompt the user for game files (ISO / XBLA / already-extracted directory),
/// extracts them if needed, and validates default.xex against an optional
/// expected SHA-256.
///
/// Designed to be called from an overridden SetupEnvironment() in the guest
/// app, after the base SetupEnvironment has loaded the config and initialized
/// logging.
///
/// Usage:
/// @code
///   bool SetupEnvironment() override {
///     if (!rex::ReXApp::SetupEnvironment()) return false;
///     rex::system::GameDataSelectorSettings settings;
///     settings.default_xex_sha256 = "aabb...";
///     settings.title_update_sha256 = "ccdd...";
///     return rex::system::GameDataSelector::EnsureGameData(settings);
///   }
/// @endcode
class GameDataSelector {
 public:
  /// Checks game_data_root via the cvar for default.xex and an optional hash
  /// match. If invalid, prompts for an ISO, XBLA package, or extracted directory,
  /// extracts the game data if needed, and sets game_data_root.
  /// Processes any required title update even when the game data is valid.
  ///
  /// Returns true if game data is ready, false on cancellation or failure.
  static bool EnsureGameData(const GameDataSelectorSettings& settings);
};

}  // namespace rex::system
