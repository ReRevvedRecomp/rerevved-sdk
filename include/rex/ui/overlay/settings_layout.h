/**
 * @file        rex/ui/overlay/settings_layout.h
 *
 * @brief       Title-supplied tabs, sections, tiers and labels for the settings overlay.
 */
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>

namespace rex::ui {

// Selectors used by sources and audience rules: an exact cvar name, a name
// prefix ending in `*` ("present_fsr_*"), or `@Category`, which also covers
// its subcategories ("@GPU" matches "GPU/D3D12").
struct SettingsAudienceRule {
  std::string match;
  rex::cvar::Audience audience;
};

struct SettingsSection {
  std::string title;  // empty: no header
  std::vector<std::string> sources;
  bool collapsed = false;  // start as a closed collapsible header; needs a title
};

struct SettingsTab {
  std::string title;
  std::vector<SettingsSection> sections;
};

struct SettingsInlineToggle {
  std::string toggle;  // bool cvar; owns the row's label, placement and audience
  std::string value;   // string or integer cvar; no separate row when paired
  bool unlock_when_checked = true;
};

// With no explicit tabs, the overlay uses its category tree. Visible rows
// not assigned by a source go to a tab named after their top-level category.
// Command cvars appear only where a source names them exactly, as a button
// labeled like other rows that invokes the command with no arguments; category
// and prefix sources skip them. Audience rules still apply, and commands are
// never saved.
struct SettingsLayout {
  std::vector<SettingsTab> tabs;
  // Overrides a row's audience; later rules win.
  std::vector<SettingsAudienceRule> audience;
  // Replaces the label derived from a cvar's description, keyed by name.
  std::unordered_map<std::string, std::string> labels;
  // Rewrites labels derived from descriptions, such as into title case;
  // `labels` entries are used as written.
  std::function<std::string(std::string_view)> format_label;
  // Display text for a row's choices, keyed by cvar name and then value.
  // A string row with allowed values shows them; unlisted values go through
  // format_label, if set. An integer row with an entry becomes a combo of
  // exactly the listed values in ascending numeric order, keyed by decimal
  // strings such as "1"; a current value that is not listed shows as a number.
  // The overlay fills "monitor" from MonitorChoiceLabels() when absent.
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>> choice_labels;
  // When set, developer rows that no source places go to this last tab, one
  // section per category, instead of their category tabs.
  std::string developer_tab;
  // False limits default tooltips to rows that do not apply immediately, and
  // shows only when the change takes effect, without description or name.
  bool show_descriptions = true;
  // Fixed decimal places for Double rows.
  // Valid values are 0..17; absent or invalid entries keep the default four.
  // Field text starts from the rounded display; parsed edits are submitted
  // without further display rounding. Config uses std::to_string (six decimals).
  std::unordered_map<std::string, int> float_decimals;
  // Each pair shows a checkbox followed by its value field under one label.
  // Search covers both cvars; visibility and placement use the toggle. Each
  // field keeps its own lifecycle and save/revert policy. Missing/wrong-type
  // or definition-hidden cvars are ignored; the first valid pair owns a cvar.
  std::vector<SettingsInlineToggle> inline_toggles;
  // Custom hover text by cvar name, independent of show_descriptions. Replaces
  // description/name and adds a colored lifecycle note only for non-live rows.
  // Shown only on the row label, including disabled rows. An inline toggle's
  // entry wins; its value's entry is used only if absent. Empty entries still
  // override.
  std::unordered_map<std::string, std::string> tooltips;
};

}  // namespace rex::ui
