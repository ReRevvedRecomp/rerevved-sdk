/**
 * @file        rex/ui/monitors.h
 *
 * @brief       Display names for the `monitor` cvar.
 */
#pragma once

#include <string>
#include <unordered_map>

namespace rex::ui {

// Value-to-text choices for the `monitor` cvar, shaped for
// SettingsLayout::choice_labels. "0" is "Default"; each connected display adds
// "<n>" -> "<n>: <name> (<width>x<height>)", with n counting displays in the
// order the window uses to resolve `monitor`. Needs the video subsystem, so
// call it once a window exists; before that only "0" is returned. Windows
// uses the monitor's friendly name, other platforms the system display name.
std::unordered_map<std::string, std::string> MonitorChoiceLabels();

}  // namespace rex::ui
