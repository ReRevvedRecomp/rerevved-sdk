
/**
 * @file        ui/monitors.cpp
 * @brief       Display names for the `monitor` cvar. See monitors.h for details.
 */

#include <rex/ui/monitors.h>

#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/platform.h>
#include <rex/string/utf8.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

namespace rex::ui {

namespace {

#if REX_PLATFORM_WIN32
// The monitor's friendly name from its active display path, or empty.
std::string FriendlyMonitorName(HMONITOR monitor) {
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (!monitor || !GetMonitorInfoW(monitor, &info)) {
    return {};
  }

  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  LONG result = ERROR_INSUFFICIENT_BUFFER;
  // The topology can change between the size query and the read.
  for (int attempt = 0; attempt < 4 && result == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) !=
        ERROR_SUCCESS) {
      return {};
    }
    paths.resize(path_count);
    modes.resize(mode_count);
    result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count,
                                modes.data(), nullptr);
    paths.resize(path_count);
  }
  if (result != ERROR_SUCCESS) {
    return {};
  }

  for (const auto& path : paths) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = path.sourceInfo.adapterId;
    source.header.id = path.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
        std::wstring_view(source.viewGdiDeviceName) != std::wstring_view(info.szDevice)) {
      continue;
    }
    DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
    target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target.header.size = sizeof(target);
    target.header.adapterId = path.targetInfo.adapterId;
    target.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) {
      return {};
    }
    const std::wstring_view name(target.monitorFriendlyDeviceName);
    return rex::string::to_utf8(
        std::u16string_view(reinterpret_cast<const char16_t*>(name.data()), name.size()));
  }
  return {};
}
#endif  // REX_PLATFORM_WIN32

std::string DisplayName(SDL_DisplayID display) {
#if REX_PLATFORM_WIN32
  const SDL_PropertiesID props = SDL_GetDisplayProperties(display);
  if (props) {
    auto* monitor = static_cast<HMONITOR>(
        SDL_GetPointerProperty(props, SDL_PROP_DISPLAY_WINDOWS_HMONITOR_POINTER, nullptr));
    if (std::string name = FriendlyMonitorName(monitor); !name.empty()) {
      return name;
    }
  }
#endif
  const char* name = SDL_GetDisplayName(display);
  return name && *name ? name : "Display";
}

}  // namespace

std::unordered_map<std::string, std::string> MonitorChoiceLabels() {
  std::unordered_map<std::string, std::string> labels{{"0", "Default"}};
  int display_count = 0;
  SDL_DisplayID* displays = SDL_GetDisplays(&display_count);
  if (!displays) {
    return labels;
  }
  // Same order and 1-based numbering as the window's `monitor` handling.
  for (int i = 0; i < display_count; ++i) {
    std::string label = std::to_string(i + 1) + ": " + DisplayName(displays[i]);
    if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(displays[i])) {
      label += " (" + std::to_string(mode->w) + "x" + std::to_string(mode->h) + ")";
    }
    labels[std::to_string(i + 1)] = std::move(label);
  }
  SDL_free(displays);
  return labels;
}

}  // namespace rex::ui
