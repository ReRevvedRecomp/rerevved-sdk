#include <catch2/catch_test_macros.hpp>

#include <cstdio>

#include <rex/ui/monitors.h>

#include "ui/overlay/settings_rows.h"

namespace {

rex::cvar::FlagEntry MakeEntry(std::string name, std::string description,
                               rex::cvar::Audience audience) {
  rex::cvar::FlagEntry entry;
  entry.name = std::move(name);
  entry.type = rex::cvar::FlagType::Boolean;
  entry.category = "Test";
  entry.description = std::move(description);
  entry.audience = audience;
  return entry;
}

}  // namespace

namespace rows = rex::ui::settings_rows;
using rex::cvar::Audience;

TEST_CASE("settings rows show rows by audience and switches", "[settings]") {
  auto visible = [](Audience audience, bool advanced, bool developer) {
    const auto entry = MakeEntry("a", "", audience);
    return rows::IsVisible(entry, audience, advanced, developer);
  };
  CHECK(visible(Audience::kPlayer, false, false));
  CHECK_FALSE(visible(Audience::kAdvanced, false, false));
  CHECK(visible(Audience::kAdvanced, true, false));
  CHECK_FALSE(visible(Audience::kDeveloper, true, false));
  CHECK(visible(Audience::kDeveloper, false, true));
  CHECK(visible(Audience::kAdvanced, false, true));
  CHECK_FALSE(visible(Audience::kHidden, true, true));
}

TEST_CASE("settings rows hide commands and single-choice rows", "[settings]") {
  auto entry = MakeEntry("a", "", Audience::kPlayer);
  entry.type = rex::cvar::FlagType::Command;
  CHECK_FALSE(rows::IsVisible(entry, Audience::kPlayer, true, true));
  entry.type = rex::cvar::FlagType::String;
  entry.constraints.allowed_values = {"bilinear"};
  CHECK_FALSE(rows::IsVisible(entry, Audience::kPlayer, true, true));
  entry.constraints.allowed_values = {"bilinear", "fsr"};
  CHECK(rows::IsVisible(entry, Audience::kPlayer, false, false));
}

TEST_CASE("settings layout shows commands only where a source names them", "[settings]") {
  auto open_logs = MakeEntry("open_log_folder", "Open log folder", Audience::kPlayer);
  open_logs.type = rex::cvar::FlagType::Command;
  open_logs.category = "Log";
  auto details = MakeEntry("dump_details", "", Audience::kPlayer);
  details.type = rex::cvar::FlagType::Command;
  details.category = "Log";
  auto level = MakeEntry("log_verbose", "", Audience::kPlayer);
  level.category = "Log";

  rex::ui::SettingsLayout layout;
  layout.tabs = {{"General", {{"Logs", {"@Log", "open_log_folder", "dump_*"}}}}};
  CHECK(rows::IsVisible(open_logs, Audience::kPlayer, false, false, layout));
  // Category and prefix sources do not reveal commands.
  CHECK_FALSE(rows::IsVisible(details, Audience::kPlayer, true, true, layout));

  // The exact source places the command even after an earlier category source.
  const std::vector<const rex::cvar::FlagEntry*> visible = {&open_logs, &level};
  const auto tabs = rows::PlaceRows(visible, layout);
  REQUIRE(tabs.size() == 1);
  REQUIRE(tabs[0].sections.size() == 1);
  CHECK(tabs[0].sections[0].rows == std::vector<const rex::cvar::FlagEntry*>{&level, &open_logs});

  // Audience rules still apply to a named command.
  layout.audience = {{"@Log", Audience::kDeveloper}};
  const auto audience = rows::EffectiveAudience(open_logs, layout);
  CHECK(audience == Audience::kDeveloper);
  CHECK_FALSE(rows::IsVisible(open_logs, audience, true, false, layout));
  CHECK(rows::IsVisible(open_logs, audience, false, true, layout));
  layout.audience = {{"open_log_folder", Audience::kHidden}};
  CHECK_FALSE(
      rows::IsVisible(open_logs, rows::EffectiveAudience(open_logs, layout), true, true, layout));
}

TEST_CASE("settings layout rules match names, prefixes and categories", "[settings]") {
  auto entry = MakeEntry("present_fsr_quality_mode", "", Audience::kAdvanced);
  entry.category = "UI/Presenter";
  CHECK(rows::Matches("present_fsr_quality_mode", entry));
  CHECK_FALSE(rows::Matches("present_fsr", entry));
  CHECK(rows::Matches("present_fsr_*", entry));
  CHECK(rows::Matches("@UI", entry));
  CHECK(rows::Matches("@UI/Presenter", entry));
  CHECK_FALSE(rows::Matches("@UI/Pres", entry));
  CHECK_FALSE(rows::Matches("@U", entry));

  rex::ui::SettingsLayout layout;
  CHECK(rows::EffectiveAudience(entry, layout) == Audience::kAdvanced);
  layout.audience = {{"@UI", Audience::kDeveloper}, {"present_fsr_*", Audience::kPlayer}};
  CHECK(rows::EffectiveAudience(entry, layout) == Audience::kPlayer);
  layout.audience.push_back({"@UI/Presenter", Audience::kHidden});
  CHECK(rows::EffectiveAudience(entry, layout) == Audience::kHidden);

  auto hidden = MakeEntry("settings_show_developer", "", Audience::kHidden);
  hidden.category = "UI/Settings";
  layout.audience = {{"@UI", Audience::kDeveloper}};
  CHECK(rows::EffectiveAudience(hidden, layout) == Audience::kHidden);
}

TEST_CASE("settings layout places rows in source order with category fallback", "[settings]") {
  auto vsync = MakeEntry("vsync", "", Audience::kPlayer);
  vsync.category = "Display";
  auto mute = MakeEntry("audio_mute", "", Audience::kPlayer);
  mute.category = "Audio";
  auto fullscreen = MakeEntry("fullscreen", "", Audience::kPlayer);
  fullscreen.category = "Display";
  auto native = MakeEntry("native_toggle", "", Audience::kPlayer);
  native.category = "Graphics/Native rendering";
  auto kernel = MakeEntry("kernel_pix", "", Audience::kAdvanced);
  kernel.category = "Kernel";
  const std::vector<const rex::cvar::FlagEntry*> visible = {&vsync, &mute, &fullscreen, &native,
                                                            &kernel};

  rex::ui::SettingsLayout layout;
  layout.tabs = {
      {"Video & Sound", {{"Video", {"fullscreen", "@Display"}}, {"Sound", {"audio_mute"}}}},
      {"Graphics", {{"", {"renderer"}}}},
      {"Empty", {{"", {"missing"}}}},
  };
  const auto tabs = rows::PlaceRows(visible, layout);
  REQUIRE(tabs.size() == 3);
  CHECK(tabs[0].title == "Video & Sound");
  REQUIRE(tabs[0].sections.size() == 2);
  CHECK(tabs[0].sections[0].rows == std::vector<const rex::cvar::FlagEntry*>{&fullscreen, &vsync});
  CHECK(tabs[0].sections[1].rows == std::vector<const rex::cvar::FlagEntry*>{&mute});
  // Unplaced rows join a same-named tab or get their own; empty tabs drop.
  CHECK(tabs[1].title == "Graphics");
  REQUIRE(tabs[1].sections.size() == 1);
  CHECK(tabs[1].sections[0].title == "Native rendering");
  CHECK(tabs[2].title == "Kernel");
  CHECK(tabs[2].sections[0].title.empty());
}

TEST_CASE("settings layout gathers unplaced developer rows in its developer tab", "[settings]") {
  auto vsync = MakeEntry("vsync", "", Audience::kPlayer);
  vsync.category = "Display";
  auto pix = MakeEntry("kernel_pix", "", Audience::kDeveloper);
  pix.category = "Kernel";
  auto dump = MakeEntry("d3d12_dump", "", Audience::kPlayer);
  dump.category = "GPU/D3D12";
  auto trace = MakeEntry("kernel_trace", "", Audience::kDeveloper);
  trace.category = "Kernel";
  auto placed = MakeEntry("native_debug", "", Audience::kDeveloper);
  placed.category = "Graphics";
  const std::vector<const rex::cvar::FlagEntry*> visible = {&vsync, &pix, &dump, &trace, &placed};

  rex::ui::SettingsLayout layout;
  layout.tabs = {{"Graphics", {{"", {"@Graphics"}}}}};
  layout.audience = {{"@GPU", Audience::kDeveloper}};
  layout.developer_tab = "Developer";
  const auto tabs = rows::PlaceRows(visible, layout);
  REQUIRE(tabs.size() == 3);
  CHECK(tabs[0].sections[0].rows == std::vector<const rex::cvar::FlagEntry*>{&placed});
  CHECK(tabs[1].title == "Display");
  CHECK(tabs[2].title == "Developer");
  REQUIRE(tabs[2].sections.size() == 2);
  CHECK(tabs[2].sections[0].title == "Kernel");
  CHECK(tabs[2].sections[0].rows == std::vector<const rex::cvar::FlagEntry*>{&pix, &trace});
  CHECK(tabs[2].sections[1].title == "GPU / D3D12");
}

TEST_CASE("settings rows format key bindings", "[settings]") {
  CHECK(rows::FormatBinding("Semicolon,Space") == "; or Space");
  CHECK(rows::FormatBinding("Shift+Up") == "Shift+Up");
  CHECK(rows::FormatBinding("Quote,Backspace") == "' or Backspace");
  CHECK(rows::FormatBinding("") == "");
}

TEST_CASE("settings rows label from the description lead", "[settings]") {
  CHECK(rows::Label(MakeEntry("fullscreen", "Fullscreen. Start the window in fullscreen mode",
                              Audience::kPlayer)) == "Fullscreen");
  CHECK(rows::Label(MakeEntry("audio_mute", "Mute audio output", Audience::kPlayer)) ==
        "Mute audio output");
  CHECK(rows::Label(MakeEntry("trailing", "Ends with a period.", Audience::kPlayer)) ==
        "Ends with a period");
  // A long lead, or none, falls back to the name.
  CHECK(rows::Label(MakeEntry("resolution_scale",
                              "Draw resolution scale for both X and Y axes (same as setting "
                              "draw_resolution_scale_x and draw_resolution_scale_y)",
                              Audience::kPlayer)) == "Resolution scale");
  CHECK(rows::Label(MakeEntry("empty_description", "", Audience::kPlayer)) == "Empty description");

  rex::ui::SettingsLayout layout;
  layout.labels["empty_description"] = "Custom";
  CHECK(rows::Label(MakeEntry("empty_description", "", Audience::kPlayer), layout) == "Custom");
}

TEST_CASE("settings layout formats derived labels but not overrides", "[settings]") {
  auto speed = MakeEntry("combat_speed", "Combat speed", Audience::kPlayer);
  auto mute = MakeEntry("audio_mute", "Mute audio", Audience::kPlayer);
  rex::ui::SettingsLayout layout;
  layout.labels = {{"audio_mute", "mute"}};
  layout.format_label = [](std::string_view label) { return "[" + std::string(label) + "]"; };
  CHECK(rows::Label(speed, layout) == "[Combat speed]");
  CHECK(rows::Label(mute, layout) == "mute");
}

TEST_CASE("settings layout labels choices by row and value", "[settings]") {
  auto effect = MakeEntry("swap_post_effect", "", Audience::kPlayer);
  auto speed = MakeEntry("combat_speed", "", Audience::kPlayer);
  rex::ui::SettingsLayout layout;
  CHECK(rows::ChoiceLabel(effect, "fxaa", layout) == "fxaa");
  layout.choice_labels = {{"swap_post_effect", {{"fxaa", "FXAA"}}}};
  layout.format_label = [](std::string_view value) { return "<" + std::string(value) + ">"; };
  CHECK(rows::ChoiceLabel(effect, "fxaa", layout) == "FXAA");
  CHECK(rows::ChoiceLabel(effect, "none", layout) == "<none>");
  CHECK(rows::ChoiceLabel(speed, "fxaa", layout) == "<fxaa>");
}

TEST_CASE("settings rows search ignores case across name, label and description", "[settings]") {
  const auto entry = MakeEntry(
      "present_effect", "Upscaling. Guest output effect: bilinear, CAS, FSR", Audience::kPlayer);
  CHECK(rows::MatchesSearch(entry, ""));
  CHECK(rows::MatchesSearch(entry, "PRESENT"));
  CHECK(rows::MatchesSearch(entry, "upscal"));
  CHECK(rows::MatchesSearch(entry, "cas"));
  CHECK(rows::MatchesSearch(entry, "Guest Output"));
  CHECK_FALSE(rows::MatchesSearch(entry, "vsync"));
}

TEST_CASE("settings rows save and revert only config-persisted visible rows", "[settings]") {
  auto entry = MakeEntry("a", "", Audience::kPlayer);
  CHECK(rows::IsSaved(entry));
  entry.persistence = rex::cvar::Persistence::kSessionOnly;
  CHECK_FALSE(rows::IsSaved(entry));
  entry.persistence = rex::cvar::Persistence::kConfig;
  entry.audience = Audience::kHidden;
  CHECK_FALSE(rows::IsSaved(entry));
  entry.audience = Audience::kAdvanced;
  entry.type = rex::cvar::FlagType::Command;
  CHECK_FALSE(rows::IsSaved(entry));
}

TEST_CASE("settings integer choices list mapped values in numeric order", "[settings]") {
  auto entry = MakeEntry("quality", "", Audience::kPlayer);
  entry.type = rex::cvar::FlagType::Int32;
  rex::ui::SettingsLayout layout;
  CHECK(rows::IntegerChoices(entry, layout).empty());

  layout.choice_labels["quality"] = {{"10", "Ultra"}, {"2", "Medium"}, {"1", "Low"}, {"x", "Bad"}};
  const auto choices = rows::IntegerChoices(entry, layout);
  REQUIRE(choices.size() == 3);
  CHECK(choices[0].value == 1);
  CHECK(choices[0].label == "Low");
  CHECK(choices[1].value == 2);
  CHECK(choices[2].value == 10);
  CHECK(choices[2].label == "Ultra");

  CHECK(rows::IntegerChoiceIndex(choices, "2") == 1);
  CHECK(rows::IntegerChoicePreview(choices, "2") == "Medium");
  // A value outside the map shows as the plain number.
  CHECK(rows::IntegerChoiceIndex(choices, "5") == choices.size());
  CHECK(rows::IntegerChoicePreview(choices, "5") == "5");
  CHECK(rows::IntegerChoicePreview(choices, "") == "");

  // Only integer rows take choices.
  entry.type = rex::cvar::FlagType::Double;
  CHECK(rows::IntegerChoices(entry, layout).empty());
  entry.type = rex::cvar::FlagType::Uint64;
  CHECK(rows::IntegerChoices(entry, layout).size() == 3);
}

TEST_CASE("settings tooltips follow show_descriptions", "[settings]") {
  using rex::cvar::Lifecycle;
  rex::ui::SettingsLayout layout;
  for (const auto lifecycle :
       {Lifecycle::kHotReload, Lifecycle::kRequiresRestart, Lifecycle::kInitOnly}) {
    const auto plan = rows::PlanRowTooltip(lifecycle, layout);
    CHECK(plan.visible);
    CHECK(plan.details);
    CHECK(plan.lifecycle);
    CHECK_FALSE(plan.text.has_value());
  }

  layout.show_descriptions = false;
  CHECK_FALSE(rows::PlanRowTooltip(Lifecycle::kHotReload, layout).visible);
  for (const auto lifecycle : {Lifecycle::kRequiresRestart, Lifecycle::kInitOnly}) {
    const auto plan = rows::PlanRowTooltip(lifecycle, layout);
    CHECK(plan.visible);
    CHECK_FALSE(plan.details);
    CHECK(plan.lifecycle);
    CHECK_FALSE(plan.text.has_value());
  }
}

TEST_CASE("settings custom tooltips override details regardless of descriptions", "[settings]") {
  using rex::cvar::Lifecycle;
  const auto entry = MakeEntry("value", "Default description", Audience::kPlayer);
  rex::ui::SettingsLayout layout;
  CHECK_FALSE(rows::RowTooltipText(entry, layout).has_value());
  layout.tooltips[entry.name] = "Custom hover text";
  const auto text = rows::RowTooltipText(entry, layout);
  REQUIRE(text.has_value());
  CHECK(*text == "Custom hover text");

  for (const bool descriptions : {false, true}) {
    layout.show_descriptions = descriptions;
    for (const auto lifecycle :
         {Lifecycle::kHotReload, Lifecycle::kRequiresRestart, Lifecycle::kInitOnly}) {
      const auto plan = rows::PlanRowTooltip(lifecycle, layout, text);
      CHECK(plan.visible);
      CHECK_FALSE(plan.details);
      CHECK(plan.lifecycle == (lifecycle != Lifecycle::kHotReload));
      CHECK(plan.text == text);
    }
  }
}

TEST_CASE("settings inline tooltip prefers toggle text and keeps each lifecycle", "[settings]") {
  using rex::cvar::Lifecycle;
  auto toggle = MakeEntry("custom_value", "Use custom value", Audience::kPlayer);
  auto value = MakeEntry("value", "Generation seed", Audience::kPlayer);
  value.type = rex::cvar::FlagType::String;
  value.lifecycle = Lifecycle::kRequiresRestart;
  const rows::InlineToggle pair{&toggle, &value, true};
  rex::ui::SettingsLayout layout;
  layout.show_descriptions = false;
  CHECK_FALSE(rows::RowTooltipText(toggle, layout, &pair).has_value());

  layout.tooltips[value.name] = "Value hover text";
  CHECK(rows::RowTooltipText(toggle, layout, &pair) == "Value hover text");
  layout.tooltips[toggle.name] = "Toggle hover text";
  const auto text = rows::RowTooltipText(toggle, layout, &pair);
  CHECK(text == "Toggle hover text");
  CHECK_FALSE(rows::PlanRowTooltip(toggle.lifecycle, layout, text).lifecycle);
  CHECK(rows::PlanRowTooltip(value.lifecycle, layout, text).lifecycle);
  // A locked value still uses the row's custom text.
  CHECK_FALSE(rows::InlineValueEnabled(pair, false));
  CHECK(rows::PlanRowTooltip(value.lifecycle, layout, text).text == text);

  // Presence, even with empty text, prevents fallback to the value's entry.
  layout.tooltips[toggle.name] = "";
  const auto empty_text = rows::RowTooltipText(toggle, layout, &pair);
  REQUIRE(empty_text.has_value());
  CHECK(empty_text->empty());
  const auto plan = rows::PlanRowTooltip(toggle.lifecycle, layout, empty_text);
  CHECK(plan.visible);
  CHECK_FALSE(plan.details);
  CHECK_FALSE(plan.lifecycle);
}

TEST_CASE("monitor choice labels always offer the default", "[settings]") {
  const auto labels = rex::ui::MonitorChoiceLabels();
  REQUIRE(labels.contains("0"));
  CHECK(labels.at("0") == "Default");
}

TEST_CASE("settings float precision changes display without rounding edits", "[settings]") {
  auto entry = MakeEntry("sensitivity", "Sensitivity", Audience::kPlayer);
  entry.type = rex::cvar::FlagType::Double;
  rex::ui::SettingsLayout layout;
  CHECK(rows::FloatFormat(entry, layout) == "%.4f");
  layout.float_decimals[entry.name] = 2;
  CHECK(rows::FloatFormat(entry, layout) == "%.2f");
  char shown[32];
  std::snprintf(shown, sizeof(shown), rows::FloatFormat(entry, layout).c_str(), 1.0);
  CHECK(std::string(shown) == "1.00");
  const double edited = 1.123456789012345;
  double parsed = 0.0;
  REQUIRE(rex::cvar::ParseDouble(rows::FloatEditText(edited), parsed));
  CHECK(parsed == edited);
  CHECK(parsed != 1.12);
  layout.float_decimals[entry.name] = 0;
  CHECK(rows::FloatFormat(entry, layout) == "%.0f");
  layout.float_decimals[entry.name] = 17;
  CHECK(rows::FloatFormat(entry, layout) == "%.17f");
  for (const int invalid : {-1, 18}) {
    layout.float_decimals[entry.name] = invalid;
    CHECK(rows::FloatFormat(entry, layout) == "%.4f");
  }
  layout.float_decimals[entry.name] = 2;
  entry.type = rex::cvar::FlagType::Int32;
  CHECK(rows::FloatFormat(entry, layout) == "%.4f");
}

TEST_CASE("settings inline toggle owns visibility placement and search", "[settings]") {
  std::vector<rex::cvar::FlagEntry> registry = {
      MakeEntry("custom_value", "Use custom value", Audience::kPlayer),
      MakeEntry("value", "Generation seed", Audience::kDeveloper)};
  registry[1].type = rex::cvar::FlagType::String;
  registry[1].category = "Other";
  rex::ui::SettingsLayout layout;
  CHECK(rows::ResolveInlineToggles(registry, layout).empty());
  layout.inline_toggles = {{"custom_value", "value", true}};
  const auto pairs = rows::ResolveInlineToggles(registry, layout);
  REQUIRE(pairs.size() == 1);
  CHECK(rows::FindInlineToggle(registry[0], pairs) == &pairs[0]);
  CHECK(rows::FindInlineToggle(registry[1], pairs) == nullptr);
  CHECK(rows::IsVisibleRow(registry[0], layout, pairs, false, false));
  CHECK_FALSE(rows::IsVisibleRow(registry[1], layout, pairs, true, true));
  CHECK(rows::MatchesSearch(pairs[0], "CUSTOM", layout));
  CHECK(rows::MatchesSearch(pairs[0], "generation", layout));
  layout.labels["value"] = "Seed text";
  CHECK(rows::MatchesSearch(pairs[0], "seed text", layout));
  CHECK_FALSE(rows::MatchesSearch(pairs[0], "absent", layout));

  layout.tabs = {{"Options", {{"", {"value", "custom_value"}}}}};
  std::vector<const rex::cvar::FlagEntry*> visible;
  for (const auto& entry : registry) {
    if (rows::IsVisibleRow(entry, layout, pairs, false, false)) {
      visible.push_back(&entry);
    }
  }
  const auto tabs = rows::PlaceRows(visible, layout);
  REQUIRE(tabs.size() == 1);
  CHECK(tabs[0].sections[0].rows == std::vector<const rex::cvar::FlagEntry*>{&registry[0]});
  layout.audience = {{"custom_value", Audience::kAdvanced}, {"value", Audience::kPlayer}};
  CHECK_FALSE(rows::IsVisibleRow(registry[0], layout, pairs, false, false));
  CHECK(rows::IsVisibleRow(registry[0], layout, pairs, true, false));
  CHECK_FALSE(rows::IsVisibleRow(registry[1], layout, pairs, true, true));
  // Pairing does not remove either cvar from the save/revert set.
  CHECK(rows::IsSaved(registry[0]));
  CHECK(rows::IsSaved(registry[1]));
}

TEST_CASE("settings inline value lock follows toggle and its own lifecycle", "[settings]") {
  std::vector<rex::cvar::FlagEntry> registry = {MakeEntry("custom_value", "", Audience::kPlayer),
                                                MakeEntry("value", "", Audience::kPlayer)};
  registry[1].type = rex::cvar::FlagType::Uint64;
  rex::ui::SettingsLayout layout;
  layout.inline_toggles = {{"custom_value", "value"}};
  auto pairs = rows::ResolveInlineToggles(registry, layout);
  REQUIRE(pairs.size() == 1);
  CHECK(rows::InlineValueEnabled(pairs[0], true));
  CHECK_FALSE(rows::InlineValueEnabled(pairs[0], false));
  pairs[0].unlock_when_checked = false;
  CHECK(rows::InlineValueEnabled(pairs[0], false));
  CHECK_FALSE(rows::InlineValueEnabled(pairs[0], true));
  registry[0].lifecycle = rex::cvar::Lifecycle::kInitOnly;
  CHECK(rows::InlineValueEnabled(pairs[0], false));
  registry[1].lifecycle = rex::cvar::Lifecycle::kRequiresRestart;
  CHECK(rows::InlineValueEnabled(pairs[0], false));
  registry[1].lifecycle = rex::cvar::Lifecycle::kInitOnly;
  CHECK_FALSE(rows::InlineValueEnabled(pairs[0], false));
}

TEST_CASE("settings inline pairs ignore invalid or already owned cvars", "[settings]") {
  std::vector<rex::cvar::FlagEntry> registry = {MakeEntry("toggle", "", Audience::kPlayer),
                                                MakeEntry("other_toggle", "", Audience::kPlayer),
                                                MakeEntry("value", "", Audience::kPlayer)};
  registry[2].type = rex::cvar::FlagType::Int32;
  rex::ui::SettingsLayout layout;
  layout.inline_toggles = {{"missing", "value"},
                           {"value", "toggle"},
                           {"toggle", "toggle"},
                           {"toggle", "value"},
                           {"other_toggle", "value"}};
  const auto pairs = rows::ResolveInlineToggles(registry, layout);
  REQUIRE(pairs.size() == 1);
  CHECK(pairs[0].toggle == &registry[0]);
  CHECK(pairs[0].value == &registry[2]);
  CHECK(rows::IsVisibleRow(registry[1], layout, pairs, false, false));
  registry[2].audience = Audience::kHidden;
  CHECK(rows::ResolveInlineToggles(registry, layout).empty());
  registry[2].audience = Audience::kPlayer;
  registry[0].audience = Audience::kHidden;
  layout.inline_toggles = {{"toggle", "value"}};
  CHECK(rows::ResolveInlineToggles(registry, layout).empty());
}
