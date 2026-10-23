
/**
 * @file        ui/overlay/settings_rows.h
 *
 * @brief       Row visibility, placement, labels and search for the settings overlay.
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/ui/overlay/settings_layout.h>

namespace rex::ui::settings_rows {

// Longest description lead used as a label; longer leads fall back to the name.
inline constexpr size_t kMaxLabelLength = 48;

// See SettingsAudienceRule for the selector forms.
inline bool Matches(std::string_view match, const rex::cvar::FlagEntry& entry) {
  if (match.starts_with('@')) {
    const std::string_view category = match.substr(1);
    return entry.category == category ||
           (entry.category.size() > category.size() && entry.category.starts_with(category) &&
            entry.category[category.size()] == '/');
  }
  if (match.ends_with('*')) {
    return std::string_view(entry.name).starts_with(match.substr(0, match.size() - 1));
  }
  return entry.name == match;
}

// Places a row: like Matches, except that a command needs its exact name, so
// category and prefix sources never turn console commands into buttons.
inline bool Places(std::string_view source, const rex::cvar::FlagEntry& entry) {
  if (entry.type == rex::cvar::FlagType::Command) {
    return entry.name == source;
  }
  return Matches(source, entry);
}

// Whether some tab source is exactly this row's name.
inline bool IsNamedBySource(const rex::cvar::FlagEntry& entry, const SettingsLayout& layout) {
  for (const auto& tab : layout.tabs) {
    for (const auto& section : tab.sections) {
      for (const auto& source : section.sources) {
        if (source == entry.name) {
          return true;
        }
      }
    }
  }
  return false;
}

inline rex::cvar::Audience EffectiveAudience(const rex::cvar::FlagEntry& entry,
                                             const SettingsLayout& layout) {
  // Rows their definition hides, such as the overlay's own switches, stay hidden.
  if (entry.audience == rex::cvar::Audience::kHidden) {
    return entry.audience;
  }
  rex::cvar::Audience audience = entry.audience;
  for (const auto& rule : layout.audience) {
    if (Matches(rule.match, entry)) {
      audience = rule.audience;
    }
  }
  return audience;
}

inline bool IsVisible(const rex::cvar::FlagEntry& entry, rex::cvar::Audience audience,
                      bool show_advanced, bool show_developer, const SettingsLayout& layout = {}) {
  // Commands belong to the console unless a tab source names them as a
  // button; a single allowed value is not a choice.
  if (entry.type == rex::cvar::FlagType::Command) {
    if (!IsNamedBySource(entry, layout)) {
      return false;
    }
  } else if (entry.constraints.allowed_values.size() == 1) {
    return false;
  }
  switch (audience) {
    case rex::cvar::Audience::kPlayer:
      return true;
    case rex::cvar::Audience::kAdvanced:
      return show_advanced || show_developer;
    case rex::cvar::Audience::kDeveloper:
      return show_developer;
    case rex::cvar::Audience::kHidden:
      return false;
  }
  return false;
}

inline std::string ToLower(std::string_view text) {
  std::string result(text);
  for (auto& c : result) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return result;
}

// "present_effect" -> "Present effect".
inline std::string PrettyName(std::string_view name) {
  std::string result(name);
  for (auto& c : result) {
    if (c == '_') {
      c = ' ';
    }
  }
  if (!result.empty()) {
    result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));
  }
  return result;
}

// The description's first sentence when it is short, otherwise the name.
inline std::string Label(const rex::cvar::FlagEntry& entry) {
  std::string_view lead = entry.description;
  if (const size_t stop = lead.find(". "); stop != std::string_view::npos) {
    lead = lead.substr(0, stop);
  }
  while (!lead.empty() && (lead.back() == '.' || lead.back() == ' ')) {
    lead.remove_suffix(1);
  }
  if (!lead.empty() && lead.size() <= kMaxLabelLength) {
    return std::string(lead);
  }
  return PrettyName(entry.name);
}

inline std::string Label(const rex::cvar::FlagEntry& entry, const SettingsLayout& layout) {
  if (auto it = layout.labels.find(entry.name); it != layout.labels.end()) {
    return it->second;
  }
  std::string label = Label(entry);
  return layout.format_label ? layout.format_label(label) : label;
}

inline std::string ChoiceLabel(const rex::cvar::FlagEntry& entry, const std::string& value,
                               const SettingsLayout& layout) {
  if (auto row = layout.choice_labels.find(entry.name); row != layout.choice_labels.end()) {
    if (auto it = row->second.find(value); it != row->second.end()) {
      return it->second;
    }
  }
  return layout.format_label ? layout.format_label(value) : value;
}

inline bool IsIntegerType(rex::cvar::FlagType type) {
  switch (type) {
    case rex::cvar::FlagType::Int32:
    case rex::cvar::FlagType::Int64:
    case rex::cvar::FlagType::Uint32:
    case rex::cvar::FlagType::Uint64:
      return true;
    default:
      return false;
  }
}

inline std::string FloatFormat(const rex::cvar::FlagEntry& entry, const SettingsLayout& layout) {
  const auto it = layout.float_decimals.find(entry.name);
  if (entry.type != rex::cvar::FlagType::Double || it == layout.float_decimals.end() ||
      it->second < 0 || it->second > std::numeric_limits<double>::max_digits10) {
    return "%.4f";
  }
  return "%." + std::to_string(it->second) + "f";
}

// Shortest representation that parses back to the edited double, independent
// of its display precision. The buffer covers every general-format double.
inline std::string FloatEditText(double value) {
  char text[64];
  const auto result = std::to_chars(text, text + sizeof(text), value);
  return std::string(text, result.ptr);
}

struct InlineToggle {
  const rex::cvar::FlagEntry* toggle;
  const rex::cvar::FlagEntry* value;
  bool unlock_when_checked;
};

inline std::vector<InlineToggle> ResolveInlineToggles(
    const std::vector<rex::cvar::FlagEntry>& registry, const SettingsLayout& layout) {
  std::vector<InlineToggle> pairs;
  std::unordered_set<const rex::cvar::FlagEntry*> used;
  for (const auto& pair : layout.inline_toggles) {
    const auto toggle = std::find_if(registry.begin(), registry.end(),
                                     [&](const auto& entry) { return entry.name == pair.toggle; });
    const auto value = std::find_if(registry.begin(), registry.end(),
                                    [&](const auto& entry) { return entry.name == pair.value; });
    if (toggle == registry.end() || value == registry.end() ||
        toggle->type != rex::cvar::FlagType::Boolean ||
        (value->type != rex::cvar::FlagType::String && !IsIntegerType(value->type)) ||
        toggle->audience == rex::cvar::Audience::kHidden ||
        value->audience == rex::cvar::Audience::kHidden || used.contains(&*toggle) ||
        used.contains(&*value)) {
      continue;
    }
    used.insert(&*toggle);
    used.insert(&*value);
    pairs.push_back({&*toggle, &*value, pair.unlock_when_checked});
  }
  return pairs;
}

inline const InlineToggle* FindInlineToggle(const rex::cvar::FlagEntry& entry,
                                            const std::vector<InlineToggle>& pairs) {
  const auto it = std::find_if(pairs.begin(), pairs.end(),
                               [&](const auto& pair) { return pair.toggle == &entry; });
  return it == pairs.end() ? nullptr : &*it;
}

inline bool IsInlineValue(const rex::cvar::FlagEntry& entry,
                          const std::vector<InlineToggle>& pairs) {
  return std::any_of(pairs.begin(), pairs.end(),
                     [&](const auto& pair) { return pair.value == &entry; });
}

inline bool IsVisibleRow(const rex::cvar::FlagEntry& entry, const SettingsLayout& layout,
                         const std::vector<InlineToggle>& pairs, bool show_advanced,
                         bool show_developer) {
  return !IsInlineValue(entry, pairs) &&
         IsVisible(entry, EffectiveAudience(entry, layout), show_advanced, show_developer, layout);
}

inline bool InlineValueEnabled(const InlineToggle& pair, bool checked) {
  return checked == pair.unlock_when_checked &&
         pair.value->lifecycle != rex::cvar::Lifecycle::kInitOnly;
}

struct IntegerChoice {
  int64_t value = 0;
  std::string label;
};

// The layout's choices for an integer row, in ascending numeric order. Keys
// that are not decimal integers are ignored; empty means a plain number input.
inline std::vector<IntegerChoice> IntegerChoices(const rex::cvar::FlagEntry& entry,
                                                 const SettingsLayout& layout) {
  std::vector<IntegerChoice> choices;
  if (!IsIntegerType(entry.type)) {
    return choices;
  }
  const auto row = layout.choice_labels.find(entry.name);
  if (row == layout.choice_labels.end()) {
    return choices;
  }
  for (const auto& [key, label] : row->second) {
    int64_t value = 0;
    const auto [end, error] = std::from_chars(key.data(), key.data() + key.size(), value);
    if (error == std::errc() && end == key.data() + key.size()) {
      choices.push_back({value, label});
    }
  }
  std::sort(choices.begin(), choices.end(),
            [](const IntegerChoice& a, const IntegerChoice& b) { return a.value < b.value; });
  return choices;
}

// Index of the choice equal to the current value, or choices.size().
inline size_t IntegerChoiceIndex(const std::vector<IntegerChoice>& choices,
                                 std::string_view current) {
  int64_t value = 0;
  const auto [end, error] = std::from_chars(current.data(), current.data() + current.size(), value);
  if (error != std::errc() || end != current.data() + current.size()) {
    return choices.size();
  }
  for (size_t i = 0; i < choices.size(); ++i) {
    if (choices[i].value == value) {
      return i;
    }
  }
  return choices.size();
}

// The combo's closed text: the current choice's label, or the plain value.
inline std::string IntegerChoicePreview(const std::vector<IntegerChoice>& choices,
                                        std::string_view current) {
  const size_t index = IntegerChoiceIndex(choices, current);
  return index < choices.size() ? choices[index].label : std::string(current);
}

// "Semicolon,Space" -> "; or Space".
inline std::string FormatBinding(std::string_view value) {
  static constexpr std::pair<std::string_view, std::string_view> kSymbols[] = {
      {"Backtick", "`"}, {"Minus", "-"},     {"Plus", "="},  {"Comma", ","},
      {"Period", "."},   {"Semicolon", ";"}, {"Slash", "/"}, {"Backslash", "\\"},
      {"LBracket", "["}, {"RBracket", "]"},  {"Quote", "'"},
  };
  std::string result;
  while (!value.empty()) {
    const size_t comma = value.find(',');
    std::string_view key = value.substr(0, comma);
    for (const auto& [name, symbol] : kSymbols) {
      if (key == name) {
        key = symbol;
        break;
      }
    }
    if (!result.empty()) {
      result += " or ";
    }
    result += key;
    value = comma == std::string_view::npos ? std::string_view() : value.substr(comma + 1);
  }
  return result;
}

// Case-insensitive substring match over the name, label and description.
inline bool MatchesSearch(const rex::cvar::FlagEntry& entry, std::string_view search,
                          const SettingsLayout& layout = {}) {
  if (search.empty()) {
    return true;
  }
  const std::string needle = ToLower(search);
  return ToLower(entry.name).find(needle) != std::string::npos ||
         ToLower(Label(entry, layout)).find(needle) != std::string::npos ||
         ToLower(entry.description).find(needle) != std::string::npos;
}

inline bool MatchesSearch(const InlineToggle& pair, std::string_view search,
                          const SettingsLayout& layout) {
  return MatchesSearch(*pair.toggle, search, layout) || MatchesSearch(*pair.value, search, layout);
}

inline const char* LifecycleText(rex::cvar::Lifecycle lifecycle) {
  switch (lifecycle) {
    case rex::cvar::Lifecycle::kHotReload:
      return "applies immediately";
    case rex::cvar::Lifecycle::kRequiresRestart:
      return "applies after restart";
    case rex::cvar::Lifecycle::kInitOnly:
      return "set at startup only";
  }
  return "";
}

// An inline toggle owns custom text for the whole row, falling back to its
// value only when the toggle has no entry. An empty entry is still an override.
inline std::optional<std::string_view> RowTooltipText(const rex::cvar::FlagEntry& entry,
                                                      const SettingsLayout& layout,
                                                      const InlineToggle* pair = nullptr) {
  const auto text = layout.tooltips.find(pair ? pair->toggle->name : entry.name);
  if (text != layout.tooltips.end()) {
    return text->second;
  }
  if (pair) {
    const auto value_text = layout.tooltips.find(pair->value->name);
    if (value_text != layout.tooltips.end()) {
      return value_text->second;
    }
  }
  return std::nullopt;
}

// Custom text replaces the default details and omits the live lifecycle note.
// Otherwise show_descriptions keeps the existing default tooltip behavior.
struct RowTooltipPlan {
  bool visible = true;
  bool details = true;  // description and cvar name, besides the lifecycle
  bool lifecycle = true;
  std::optional<std::string_view> text;
};

inline RowTooltipPlan PlanRowTooltip(rex::cvar::Lifecycle lifecycle, const SettingsLayout& layout,
                                     std::optional<std::string_view> text = std::nullopt) {
  if (text) {
    return {true, false, lifecycle != rex::cvar::Lifecycle::kHotReload, text};
  }
  if (layout.show_descriptions) {
    return {true, true, true, std::nullopt};
  }
  return {lifecycle != rex::cvar::Lifecycle::kHotReload, false, true, std::nullopt};
}

// Rows that Save writes and Revert restores.
inline bool IsSaved(const rex::cvar::FlagEntry& entry) {
  return entry.type != rex::cvar::FlagType::Command &&
         entry.persistence == rex::cvar::Persistence::kConfig &&
         entry.audience != rex::cvar::Audience::kHidden;
}

struct PlacedSection {
  std::string title;
  bool collapsed = false;
  std::vector<const rex::cvar::FlagEntry*> rows;
};

struct PlacedTab {
  std::string title;
  std::vector<PlacedSection> sections;
};

// Places rows into the layout's tabs in source order, then puts unplaced rows
// in a tab named after their top-level category, or unplaced developer rows in
// the layout's developer tab when it has one. Empty tabs are dropped.
inline std::vector<PlacedTab> PlaceRows(const std::vector<const rex::cvar::FlagEntry*>& rows,
                                        const SettingsLayout& layout) {
  std::vector<PlacedTab> tabs;
  std::unordered_set<const rex::cvar::FlagEntry*> placed;
  for (const auto& tab : layout.tabs) {
    PlacedTab& placed_tab = tabs.emplace_back(PlacedTab{tab.title, {}});
    for (const auto& section : tab.sections) {
      PlacedSection placed_section{section.title, section.collapsed, {}};
      for (const auto& source : section.sources) {
        for (const auto* row : rows) {
          if (!placed.contains(row) && Places(source, *row)) {
            placed.insert(row);
            placed_section.rows.push_back(row);
          }
        }
      }
      if (!placed_section.rows.empty()) {
        placed_tab.sections.push_back(std::move(placed_section));
      }
    }
  }
  PlacedTab developer{layout.developer_tab, {}};
  for (const auto* row : rows) {
    if (placed.contains(row)) {
      continue;
    }
    if (!layout.developer_tab.empty() &&
        EffectiveAudience(*row, layout) == rex::cvar::Audience::kDeveloper) {
      std::string title = row->category;
      for (size_t pos = 0; (pos = title.find('/', pos)) != std::string::npos; pos += 3) {
        title.replace(pos, 1, " / ");
      }
      auto section = std::find_if(
          developer.sections.begin(), developer.sections.end(),
          [&title](const PlacedSection& candidate) { return candidate.title == title; });
      if (section == developer.sections.end()) {
        section = developer.sections.insert(developer.sections.end(),
                                            PlacedSection{std::move(title), false, {}});
      }
      section->rows.push_back(row);
      continue;
    }
    const size_t slash = row->category.find('/');
    const std::string top = row->category.substr(0, slash);
    const std::string rest =
        slash == std::string::npos ? std::string() : row->category.substr(slash + 1);
    auto tab = std::find_if(tabs.begin(), tabs.end(),
                            [&top](const PlacedTab& candidate) { return candidate.title == top; });
    if (tab == tabs.end()) {
      tab = tabs.insert(tabs.end(), PlacedTab{top, {}});
    }
    auto section =
        std::find_if(tab->sections.begin(), tab->sections.end(),
                     [&rest](const PlacedSection& candidate) { return candidate.title == rest; });
    if (section == tab->sections.end()) {
      section = tab->sections.insert(tab->sections.end(), PlacedSection{rest, false, {}});
    }
    section->rows.push_back(row);
  }
  tabs.push_back(std::move(developer));
  std::erase_if(tabs, [](const PlacedTab& tab) { return tab.sections.empty(); });
  return tabs;
}

}  // namespace rex::ui::settings_rows
