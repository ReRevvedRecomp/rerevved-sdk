#include <rex/system/profile.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <fstream>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/crypto/sha256.h>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace rex::system {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kDefaultProfileAlias = "default";
constexpr std::string_view kProfilesDirectory = "profiles";
constexpr std::string_view kHeadersDirectory = "Headers";
constexpr std::string_view kMarketplaceContentType = "00000002";
constexpr std::string_view kAchievementDirectory = "achievements";
constexpr std::string_view kModLoadoutFile = "mod-loadout.toml";

enum class EntryState {
  kMissing,
  kRegular,
  kDirectory,
  kLink,
  kOther,
  kError,
};

bool IsWindowsReparsePoint(const fs::path& path) {
#if defined(_WIN32)
  const auto attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  (void)path;
  return false;
#endif
}

EntryState Inspect(const fs::path& path) {
  std::error_code ec;
  const auto status = fs::symlink_status(path, ec);
  if (ec == std::errc::no_such_file_or_directory) {
    return EntryState::kMissing;
  }
  if (ec) {
    return EntryState::kError;
  }
  if (fs::is_symlink(status) || IsWindowsReparsePoint(path)) {
    return EntryState::kLink;
  }
  if (fs::is_regular_file(status)) {
    return EntryState::kRegular;
  }
  if (fs::is_directory(status)) {
    return EntryState::kDirectory;
  }
  return EntryState::kOther;
}

// Checks each existing path component without resolving links. A link is
// never allowed to become part of a profile root or transaction path.
bool HasLinkOrReparsePoint(const fs::path& path) {
  std::error_code ec;
  auto absolute = fs::absolute(path, ec);
  if (ec) {
    return true;
  }
  absolute = absolute.lexically_normal();

  fs::path current = absolute.root_path();
  for (const auto& component : absolute.relative_path()) {
    current /= component;
    const auto state = Inspect(current);
    if (state == EntryState::kLink || state == EntryState::kError) {
      return true;
    }
  }
  return false;
}

std::optional<fs::path> WeaklyCanonical(const fs::path& path) {
  std::error_code ec;
  const auto canonical = fs::weakly_canonical(path, ec);
  if (ec) {
    return std::nullopt;
  }
  return canonical.lexically_normal();
}

bool IsContained(const fs::path& root, const fs::path& candidate) {
  if (HasLinkOrReparsePoint(root) || HasLinkOrReparsePoint(candidate)) {
    return false;
  }
  const auto canonical_root = WeaklyCanonical(root);
  const auto canonical_candidate = WeaklyCanonical(candidate);
  if (!canonical_root || !canonical_candidate) {
    return false;
  }

  const auto relative = canonical_candidate->lexically_relative(*canonical_root);
  if (relative.empty()) {
    return *canonical_candidate == *canonical_root;
  }
  if (relative.is_absolute()) {
    return false;
  }
  const auto first = *relative.begin();
  return first != fs::path("..") && first != fs::path(".");
}

bool IsSafeRelativePath(const fs::path& path) {
  if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
    return false;
  }

  for (const auto& component : path) {
    const auto value = component.generic_string();
    if (value.empty() || value == "." || value == ".." || value.find(':') != std::string::npos ||
        value.ends_with(' ') || value.ends_with('.')) {
      return false;
    }
  }
  return true;
}

bool IsDeviceName(std::string_view id) {
  if (id == "con" || id == "prn" || id == "aux" || id == "nul") {
    return true;
  }
  if (id.size() == 4 && (id.starts_with("com") || id.starts_with("lpt")) && id[3] >= '1' &&
      id[3] <= '9') {
    return true;
  }
  return false;
}

struct FileToCopy {
  fs::path source;
  fs::path relative;
};

enum class CollectResult {
  kSuccess,
  kMissing,
  kUnsafe,
  kIoError,
  kDestinationCollision,
};

enum class AddFileResult {
  kSuccess,
  kInvalid,
  kDuplicate,
};

AddFileResult AddFile(std::vector<FileToCopy>& files, std::unordered_set<std::string>& destinations,
                      const fs::path& source, const fs::path& relative) {
  if (!IsSafeRelativePath(relative) || !IsContained(source.parent_path(), source)) {
    return AddFileResult::kInvalid;
  }
  if (!destinations.emplace(relative.generic_string()).second) {
    return AddFileResult::kDuplicate;
  }
  files.push_back({source, relative});
  return AddFileResult::kSuccess;
}

CollectResult CollectTree(const fs::path& root, const fs::path& relative_root,
                          std::vector<FileToCopy>& files,
                          std::unordered_set<std::string>& destinations) {
  if (HasLinkOrReparsePoint(root)) {
    return CollectResult::kUnsafe;
  }
  const auto state = Inspect(root);
  if (state == EntryState::kMissing) {
    return CollectResult::kMissing;
  }
  if (state == EntryState::kLink || state == EntryState::kOther) {
    return CollectResult::kUnsafe;
  }
  if (state == EntryState::kError) {
    return CollectResult::kIoError;
  }
  if (state == EntryState::kRegular) {
    const auto added = AddFile(files, destinations, root, relative_root);
    return added == AddFileResult::kSuccess     ? CollectResult::kSuccess
           : added == AddFileResult::kDuplicate ? CollectResult::kDestinationCollision
                                                : CollectResult::kUnsafe;
  }

  std::error_code ec;
  fs::recursive_directory_iterator iterator(root, fs::directory_options::none, ec);
  if (ec) {
    return CollectResult::kIoError;
  }
  while (iterator != fs::recursive_directory_iterator()) {
    const auto entry_path = iterator->path();
    const auto entry_state = Inspect(entry_path);
    if (entry_state == EntryState::kLink || entry_state == EntryState::kOther) {
      return CollectResult::kUnsafe;
    }
    if (entry_state == EntryState::kError) {
      return CollectResult::kIoError;
    }
    if (entry_state == EntryState::kDirectory) {
      iterator.increment(ec);
      if (ec) {
        return CollectResult::kIoError;
      }
      continue;
    }
    if (entry_state != EntryState::kRegular) {
      return CollectResult::kIoError;
    }
    const auto relative = relative_root / entry_path.lexically_relative(root);
    const auto added = AddFile(files, destinations, entry_path, relative);
    if (added != AddFileResult::kSuccess) {
      return added == AddFileResult::kDuplicate ? CollectResult::kDestinationCollision
                                                : CollectResult::kUnsafe;
    }
    iterator.increment(ec);
    if (ec) {
      return CollectResult::kIoError;
    }
  }
  return CollectResult::kSuccess;
}

CollectResult CollectOptionalFile(const fs::path& source, const fs::path& relative,
                                  std::vector<FileToCopy>& files,
                                  std::unordered_set<std::string>& destinations) {
  if (HasLinkOrReparsePoint(source)) {
    return CollectResult::kUnsafe;
  }
  const auto state = Inspect(source);
  if (state == EntryState::kMissing) {
    return CollectResult::kMissing;
  }
  if (state == EntryState::kLink || state == EntryState::kOther) {
    return CollectResult::kUnsafe;
  }
  if (state != EntryState::kRegular) {
    return state == EntryState::kError ? CollectResult::kIoError : CollectResult::kUnsafe;
  }
  const auto added = AddFile(files, destinations, source, relative);
  if (added != AddFileResult::kSuccess) {
    return added == AddFileResult::kDuplicate ? CollectResult::kDestinationCollision
                                              : CollectResult::kUnsafe;
  }
  return CollectResult::kSuccess;
}

CollectResult CollectOptionalDirectory(const fs::path& source, const fs::path& relative,
                                       std::vector<FileToCopy>& files,
                                       std::unordered_set<std::string>& destinations) {
  if (HasLinkOrReparsePoint(source)) {
    return CollectResult::kUnsafe;
  }
  const auto state = Inspect(source);
  if (state == EntryState::kMissing) {
    return CollectResult::kMissing;
  }
  if (state != EntryState::kDirectory) {
    return state == EntryState::kLink || state == EntryState::kOther ? CollectResult::kUnsafe
                                                                     : CollectResult::kIoError;
  }
  return CollectTree(source, relative, files, destinations);
}

bool IsMarketplaceType(const fs::path& type_path) {
  return type_path.filename().generic_string() == kMarketplaceContentType;
}

CollectResult CollectContent(const fs::path& base_root, uint32_t title_id,
                             std::vector<FileToCopy>& files,
                             std::unordered_set<std::string>& destinations) {
  const auto xuid = fmt::format("{:016X}", kBaselineProfileXuid);
  const auto title = fmt::format("{:08X}", title_id);
  const auto title_root = base_root / xuid / title;
  if (HasLinkOrReparsePoint(title_root)) {
    return CollectResult::kUnsafe;
  }
  const auto state = Inspect(title_root);
  if (state == EntryState::kMissing) {
    return CollectResult::kMissing;
  }
  if (state != EntryState::kDirectory) {
    return state == EntryState::kLink || state == EntryState::kOther ? CollectResult::kUnsafe
                                                                     : CollectResult::kIoError;
  }

  std::error_code ec;
  fs::directory_iterator types(title_root, fs::directory_options::none, ec);
  if (ec) {
    return CollectResult::kIoError;
  }
  while (types != fs::directory_iterator()) {
    const auto type_path = types->path();
    if (type_path.filename() != kHeadersDirectory && !IsMarketplaceType(type_path)) {
      const auto type_state = Inspect(type_path);
      if (type_state != EntryState::kDirectory) {
        return type_state == EntryState::kLink || type_state == EntryState::kOther
                   ? CollectResult::kUnsafe
                   : CollectResult::kIoError;
      }
      const auto relative = fs::path(xuid) / title / type_path.filename();
      const auto result = CollectTree(type_path, relative, files, destinations);
      if (result != CollectResult::kSuccess && result != CollectResult::kMissing) {
        return result;
      }
    }
    types.increment(ec);
    if (ec) {
      return CollectResult::kIoError;
    }
  }
  return CollectResult::kSuccess;
}

CollectResult CollectHeaders(const fs::path& base_root, uint32_t title_id,
                             std::vector<FileToCopy>& files,
                             std::unordered_set<std::string>& destinations) {
  const auto xuid = fmt::format("{:016X}", kBaselineProfileXuid);
  const auto title = fmt::format("{:08X}", title_id);
  const auto title_root = base_root / xuid / title;
  const auto headers_root = title_root / kHeadersDirectory;
  if (HasLinkOrReparsePoint(headers_root)) {
    return CollectResult::kUnsafe;
  }
  const auto state = Inspect(headers_root);
  if (state == EntryState::kMissing) {
    return CollectResult::kMissing;
  }
  if (state != EntryState::kDirectory) {
    return state == EntryState::kLink || state == EntryState::kOther ? CollectResult::kUnsafe
                                                                     : CollectResult::kIoError;
  }

  std::error_code ec;
  fs::directory_iterator types(headers_root, fs::directory_options::none, ec);
  if (ec) {
    return CollectResult::kIoError;
  }
  while (types != fs::directory_iterator()) {
    const auto type_path = types->path();
    if (IsMarketplaceType(type_path)) {
      types.increment(ec);
      if (ec) {
        return CollectResult::kIoError;
      }
      continue;
    }
    if (Inspect(type_path) != EntryState::kDirectory) {
      return Inspect(type_path) == EntryState::kLink ? CollectResult::kUnsafe
                                                     : CollectResult::kIoError;
    }

    const auto package_root = title_root / type_path.filename();
    fs::recursive_directory_iterator iterator(type_path, fs::directory_options::none, ec);
    if (ec) {
      return CollectResult::kIoError;
    }
    while (iterator != fs::recursive_directory_iterator()) {
      const auto entry_path = iterator->path();
      const auto entry_state = Inspect(entry_path);
      if (entry_state == EntryState::kLink || entry_state == EntryState::kOther) {
        return CollectResult::kUnsafe;
      }
      if (entry_state == EntryState::kError) {
        return CollectResult::kIoError;
      }
      if (entry_state != EntryState::kRegular) {
        iterator.increment(ec);
        if (ec) {
          return CollectResult::kIoError;
        }
        continue;
      }

      const auto filename = entry_path.filename().generic_string();
      constexpr std::string_view kHeaderSuffix = ".header";
      if (!filename.ends_with(kHeaderSuffix)) {
        iterator.increment(ec);
        if (ec) {
          return CollectResult::kIoError;
        }
        continue;
      }
      const auto package_name = filename.substr(0, filename.size() - kHeaderSuffix.size());
      const auto package =
          package_root / entry_path.lexically_relative(type_path).parent_path() / package_name;
      const auto package_state = Inspect(package);
      if (package_state == EntryState::kLink || package_state == EntryState::kOther ||
          package_state == EntryState::kError) {
        return package_state == EntryState::kLink || package_state == EntryState::kOther
                   ? CollectResult::kUnsafe
                   : CollectResult::kIoError;
      }
      if (package_state == EntryState::kMissing) {
        iterator.increment(ec);
        if (ec) {
          return CollectResult::kIoError;
        }
        continue;
      }
      const auto relative = fs::path(xuid) / title / kHeadersDirectory / type_path.filename() /
                            entry_path.lexically_relative(type_path);
      const auto added = AddFile(files, destinations, entry_path, relative);
      if (added != AddFileResult::kSuccess) {
        return added == AddFileResult::kDuplicate ? CollectResult::kDestinationCollision
                                                  : CollectResult::kUnsafe;
      }
      iterator.increment(ec);
      if (ec) {
        return CollectResult::kIoError;
      }
    }
    types.increment(ec);
    if (ec) {
      return CollectResult::kIoError;
    }
  }
  return CollectResult::kSuccess;
}

CollectResult CollectAllowlist(const ProfilePaths& target,
                               const ProfileCopySpecification& specification,
                               std::vector<FileToCopy>& files) {
  std::unordered_set<std::string> destinations;
  const auto config_source = target.base_root / specification.config_relative_path;
  auto result =
      CollectOptionalFile(config_source, specification.config_relative_path, files, destinations);
  if (result != CollectResult::kSuccess && result != CollectResult::kMissing) {
    return result;
  }

  result = CollectContent(target.base_root, specification.title_id, files, destinations);
  if (result != CollectResult::kSuccess && result != CollectResult::kMissing) {
    return result;
  }
  result = CollectHeaders(target.base_root, specification.title_id, files, destinations);
  if (result != CollectResult::kSuccess && result != CollectResult::kMissing) {
    return result;
  }

  const auto title = fmt::format("{:08X}", specification.title_id);
  result = CollectOptionalDirectory(target.base_root / title / "profile" / "User",
                                    fs::path(title) / "profile" / "User", files, destinations);
  if (result != CollectResult::kSuccess && result != CollectResult::kMissing) {
    return result;
  }
  result =
      CollectOptionalFile(target.base_root / kAchievementDirectory / (title + ".toml"),
                          fs::path(kAchievementDirectory) / (title + ".toml"), files, destinations);
  if (result != CollectResult::kSuccess && result != CollectResult::kMissing) {
    return result;
  }
  result = CollectOptionalFile(target.base_root / kModLoadoutFile, fs::path(kModLoadoutFile), files,
                               destinations);
  if (result != CollectResult::kSuccess && result != CollectResult::kMissing) {
    return result;
  }
  return CollectResult::kSuccess;
}

bool TargetIsAbsent(const fs::path& target) {
  return Inspect(target) == EntryState::kMissing;
}

bool CopyAndVerify(const FileToCopy& file, const fs::path& stage) {
  if (Inspect(file.source) != EntryState::kRegular) {
    return false;
  }
  std::error_code ec;
  const auto source_size = fs::file_size(file.source, ec);
  if (ec) {
    return false;
  }
  const auto source_hash = rex::crypto::sha256_file(file.source);
  if (source_hash.empty()) {
    return false;
  }

  const auto destination = stage / file.relative;
  if (!IsContained(stage, destination) || !TargetIsAbsent(destination)) {
    return false;
  }
  fs::create_directories(destination.parent_path(), ec);
  if (ec || HasLinkOrReparsePoint(destination.parent_path())) {
    return false;
  }

  std::ifstream input(file.source, std::ios::binary);
  std::ofstream output(destination, std::ios::binary | std::ios::trunc);
  if (!input || !output) {
    return false;
  }
  std::array<char, 256 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) {
      output.write(buffer.data(), count);
    }
  }
  if (!input.eof() || !output) {
    return false;
  }
  output.flush();
  output.close();
  if (!output) {
    return false;
  }

  const auto destination_size = fs::file_size(destination, ec);
  if (ec || destination_size != source_size) {
    return false;
  }
  if (rex::crypto::sha256_file(destination) != source_hash) {
    return false;
  }
  // Re-read the source digest to reject a source that changed during the copy.
  return Inspect(file.source) == EntryState::kRegular &&
         fs::file_size(file.source, ec) == source_size &&
         rex::crypto::sha256_file(file.source) == source_hash;
}

void RemoveConfinedTree(const fs::path& path) {
  const auto state = Inspect(path);
  if (state == EntryState::kMissing) {
    return;
  }
  if (state == EntryState::kLink || state == EntryState::kOther || state == EntryState::kError) {
    std::error_code ec;
    fs::remove(path, ec);
    return;
  }
  if (state == EntryState::kRegular) {
    std::error_code ec;
    fs::remove(path, ec);
    return;
  }

  std::error_code ec;
  fs::directory_iterator iterator(path, fs::directory_options::none, ec);
  if (ec) {
    return;
  }
  while (iterator != fs::directory_iterator()) {
    const auto entry_path = iterator->path();
    RemoveConfinedTree(entry_path);
    iterator.increment(ec);
    if (ec) {
      return;
    }
  }
  fs::remove(path, ec);
}

class StagingGuard {
 public:
  explicit StagingGuard(fs::path path) : path_(std::move(path)) {}
  ~StagingGuard() {
    if (!published_ && IsContained(path_.parent_path(), path_) &&
        !HasLinkOrReparsePoint(path_.parent_path())) {
      RemoveConfinedTree(path_);
    }
  }
  void Publish() { published_ = true; }

 private:
  fs::path path_;
  bool published_ = false;
};

std::optional<fs::path> CreateStagingDirectory(const fs::path& profiles_root,
                                               std::string_view profile_id) {
  static std::atomic<uint64_t> next_id{0};
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  for (uint32_t attempt = 0; attempt < 100; ++attempt) {
    const auto name = std::string(".") + std::string(profile_id) + ".staging-" +
                      std::to_string(now) + "-" + std::to_string(next_id.fetch_add(1));
    const auto path = profiles_root / name;
    if (!TargetIsAbsent(path)) {
      continue;
    }
    std::error_code ec;
    fs::create_directory(path, ec);
    if (!ec && Inspect(path) == EntryState::kDirectory && IsContained(profiles_root, path) &&
        !HasLinkOrReparsePoint(path)) {
      return path;
    }
  }
  return std::nullopt;
}

}  // namespace

bool IsValidProfileId(std::string_view id) {
  if (id.empty() || id.size() > 32 || id == kDefaultProfileAlias || IsDeviceName(id)) {
    return false;
  }
  const auto first = id.front();
  if (!((first >= 'a' && first <= 'z') || (first >= '0' && first <= '9'))) {
    return false;
  }
  return std::all_of(id.begin() + 1, id.end(), [](const char character) {
    return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
           character == '_' || character == '-';
  });
}

std::optional<ProfilePaths> ResolveProfile(const fs::path& base_root, std::string_view profile_id) {
  if (base_root.empty()) {
    return std::nullopt;
  }
  const bool is_default = profile_id.empty() || profile_id == kDefaultProfileAlias;
  if (!is_default && !IsValidProfileId(profile_id)) {
    return std::nullopt;
  }

  const auto normalized_base = base_root.lexically_normal();
  if (HasLinkOrReparsePoint(normalized_base)) {
    return std::nullopt;
  }

  ProfilePaths result;
  result.base_root = normalized_base;
  result.profiles_root = normalized_base / kProfilesDirectory;
  result.profile_id = is_default ? std::string{} : std::string(profile_id);
  result.active_root = is_default ? normalized_base : result.profiles_root / result.profile_id;

  if (!is_default) {
    if (!IsContained(result.profiles_root, result.active_root) ||
        HasLinkOrReparsePoint(result.profiles_root)) {
      return std::nullopt;
    }
    const auto profiles_state = Inspect(result.profiles_root);
    if (profiles_state != EntryState::kMissing && profiles_state != EntryState::kDirectory) {
      return std::nullopt;
    }
    const auto active_state = Inspect(result.active_root);
    if (active_state == EntryState::kLink || active_state == EntryState::kOther ||
        active_state == EntryState::kError ||
        (active_state != EntryState::kMissing && active_state != EntryState::kDirectory)) {
      return std::nullopt;
    }
  }
  return result;
}

ProfileCopyResult CopyFromDefault(const ProfilePaths& target,
                                  const ProfileCopySpecification& specification) {
  if (specification.title_id == 0 || !IsSafeRelativePath(specification.config_relative_path)) {
    return ProfileCopyResult::kInvalidSpecification;
  }

  const auto resolved = ResolveProfile(target.base_root, target.profile_id);
  if (!resolved ||
      resolved->active_root.lexically_normal() != target.active_root.lexically_normal()) {
    return ProfileCopyResult::kInvalidProfile;
  }
  if (resolved->is_default()) {
    return ProfileCopyResult::kDefaultProfile;
  }
  if (!IsContained(resolved->base_root, resolved->active_root) ||
      !IsContained(resolved->profiles_root, resolved->active_root) ||
      HasLinkOrReparsePoint(resolved->base_root) ||
      HasLinkOrReparsePoint(resolved->profiles_root)) {
    return ProfileCopyResult::kInvalidProfile;
  }
  if (!TargetIsAbsent(resolved->active_root)) {
    return ProfileCopyResult::kTargetExists;
  }

  std::vector<FileToCopy> files;
  const auto collected = CollectAllowlist(*resolved, specification, files);
  if (collected == CollectResult::kUnsafe) {
    return ProfileCopyResult::kUnsafeSource;
  }
  if (collected == CollectResult::kIoError) {
    return ProfileCopyResult::kIoError;
  }
  if (collected == CollectResult::kDestinationCollision) {
    return ProfileCopyResult::kDestinationCollision;
  }

  std::error_code ec;
  const auto profiles_state = Inspect(resolved->profiles_root);
  if (profiles_state == EntryState::kLink || profiles_state == EntryState::kOther ||
      profiles_state == EntryState::kError) {
    return ProfileCopyResult::kUnsafeSource;
  }
  if (profiles_state == EntryState::kMissing) {
    fs::create_directories(resolved->profiles_root, ec);
    if (ec) {
      return ProfileCopyResult::kIoError;
    }
  }
  if (HasLinkOrReparsePoint(resolved->profiles_root) ||
      !IsContained(resolved->base_root, resolved->profiles_root)) {
    return ProfileCopyResult::kUnsafeSource;
  }

  const auto staging = CreateStagingDirectory(resolved->profiles_root, resolved->profile_id);
  if (!staging) {
    return ProfileCopyResult::kIoError;
  }
  StagingGuard guard(*staging);

  for (const auto& file : files) {
    if (!CopyAndVerify(file, *staging)) {
      return ProfileCopyResult::kVerificationFailed;
    }
  }
  if (!TargetIsAbsent(resolved->active_root)) {
    return ProfileCopyResult::kTargetExists;
  }
  if (!IsContained(resolved->profiles_root, resolved->active_root) ||
      HasLinkOrReparsePoint(resolved->profiles_root)) {
    return ProfileCopyResult::kInvalidProfile;
  }

  fs::rename(*staging, resolved->active_root, ec);
  if (ec) {
    return TargetIsAbsent(resolved->active_root) ? ProfileCopyResult::kIoError
                                                 : ProfileCopyResult::kTargetExists;
  }
  guard.Publish();
  return ProfileCopyResult::kSuccess;
}

}  // namespace rex::system
