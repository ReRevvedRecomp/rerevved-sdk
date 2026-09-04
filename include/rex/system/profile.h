/**
 * @file        rex/system/profile.h
 * @brief       Host profile selection and default-profile copy helpers.
 *
 * Profiles select host storage only. They do not alter guest identity or
 * content formats.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace rex::system {

/// The default local user used by ReXGlue's existing content layout.
inline constexpr uint64_t kBaselineProfileXuid = 0xB13EBABEBABEBABEull;

/// Returns true only for a canonical, non-reserved profile ID.
bool IsValidProfileId(std::string_view id);

/// Resolved host roots. An empty profile_id denotes the established default.
struct ProfilePaths {
  std::filesystem::path base_root;
  std::filesystem::path active_root;
  std::filesystem::path profiles_root;
  std::string profile_id;

  bool is_default() const { return profile_id.empty(); }
};

/// Resolves an empty/default selection to base_root and a named selection to
/// base_root/profiles/<id>. This function never creates or modifies paths.
std::optional<ProfilePaths> ResolveProfile(const std::filesystem::path& base_root,
                                           std::string_view profile_id);

/// Facts a title may provide to the generic copy engine. Both paths are
/// interpreted relative to the default base root; no title path is copied
/// without confinement checks.
struct ProfileCopySpecification {
  std::filesystem::path config_relative_path;
  uint32_t title_id = 0;
};

enum class ProfileCopyResult {
  kSuccess,
  kInvalidProfile,
  kDefaultProfile,
  kTargetExists,
  kInvalidSpecification,
  kUnsafeSource,
  kDestinationCollision,
  kIoError,
  kVerificationFailed,
};

/// Copies the SDK allowlist from the default root into a new named profile.
/// The destination must be absent. The operation publishes once, by rename,
/// after all copied files have passed size and SHA-256 verification.
ProfileCopyResult CopyFromDefault(const ProfilePaths& target,
                                  const ProfileCopySpecification& specification);

}  // namespace rex::system
