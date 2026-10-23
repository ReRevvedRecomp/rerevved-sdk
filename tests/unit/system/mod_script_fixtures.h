#pragma once
#include <string_view>
inline constexpr std::string_view kHills = R"lua(local production_bonus = 1

return {
    provider_id = "aeshur.hills-production",
    rules = {
        {
            id = "hills-production", kind = "terrain_yield",
            terrain = "hill", component = "production",
            operation = "add", value = production_bonus,
        },
    },
}
)lua";
inline constexpr std::string_view kJaguar = R"lua(local forest_bonus = 50
local sign = forest_bonus >= 0 and "+" or ""

return {
    provider_id = "aeshur.jaguar-woodsman",
    rules = {
        {
            id = "jaguar-warrior-forest-attack", kind = "unit_combat",
            civilization = "aztec", base_unit_type = "warrior",
            identity = "jaguar_warrior", terrain = "forest",
            property = "attack", percentage_delta = forest_bonus,
        },
        {
            id = "jaguar-warrior-forest-defense", kind = "unit_combat",
            civilization = "aztec", base_unit_type = "warrior",
            identity = "jaguar_warrior", terrain = "forest",
            property = "defense", percentage_delta = forest_bonus,
        },
        {
            id = "jaguar-woodsman-text", kind = "nation_select_text",
            surface = "unique_unit", civilization = "aztec",
            base_unit_type = "warrior", identity = "jaguar_warrior",
            display_form = "unit",
            text = "Jaguar Warrior - Warrior with " .. sign .. forest_bonus
                .. "% Attack and Defense in Forest",
        },
    },
}
)lua";
