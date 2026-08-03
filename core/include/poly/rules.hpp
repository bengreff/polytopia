// Rule parameters. Structure lives in code; every NUMBER lives here and is loaded from
// data/rules/*.json (docs/phase0-plan.md §3).
//
// Rules is deliberately NOT part of GameState. It is passed as a const reference to every
// function, so one instance serves thousands of parallel environments and state clones stay
// small. It is loaded once per process and never mutated during play.
//
// Built-in defaults mirror docs/rules-research/live-rules.md so the engine is runnable
// without a data file. THE JSON IS AUTHORITATIVE: anything present there overwrites the
// default. The defaults exist for tests and for a missing-file fallback, not as a second
// source of truth -- when they disagree with the JSON, the JSON is right and the default is
// stale.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "poly/content.hpp"
#include "poly/types.hpp"

namespace poly {

// Terrain membership as a bitmask, so "buildable on Field or Forest" is one word.
constexpr std::uint32_t terrain_bit(Terrain t) { return 1u << static_cast<int>(t); }

struct UnitStats {
  int cost{2};
  int hp{10};          // whole HP; the engine scales by kHpScale on spawn
  float attack{2.0f};  // fractional: Knight 3.5, Cloak 0.5
  float defence{2.0f};
  int move{1};
  int range{1};
  int vision{1};
  Tech tech{Tech::Climbing};
  bool needs_tech{false};
  bool trainable{true};
  bool can_promote{true};
  std::uint32_t skills{0};
};

struct ImprovementStats {
  int cost{0};
  int population{0};
  Tech tech{Tech::Climbing};
  bool needs_tech{true};
  std::uint32_t terrain_mask{0};
  bool needs_resource{false};
  Resource resource{Resource::None};
  int per_city_limit{0};  // 0 = unlimited
  // Output scales with adjacent friendly buildings of this type (Sawmill/Windmill/Forge).
  Improvement adjacency_source{Improvement::None};
  int adjacency_population{0};
};

struct ResourceStats {
  int harvest_cost{0};
  int population{0};
  int stars{0};
  Tech tech{Tech::Climbing};
  bool needs_tech{true};
  bool harvestable{true};
};

struct TribeStats {
  Tech start_tech{Tech::Climbing};
  UnitType start_unit{UnitType::Warrior};
  int capital_level{1};  // Luxidoor is 3 (resolutions.md R12)
  // Varies BY TRIBE, balanced against starting tech. The wiki's flat 5 is stale; the
  // official 2025 balance pass is the source (resolutions.md C2). Luxidoor's 2 is the
  // trade against its level-3 capital, which independently corroborates R12.
  int start_stars{5};
  bool has_start_tech{true};  // Luxidoor starts with none
};

struct Rules {
  std::string version{"builtin-default"};

  // --- Combat (live-rules.md §1) ---
  float accelerator{4.5f};             // the 4.5 constant
  float defence_bonus_none{1.0f};
  float defence_bonus_standard{1.5f};  // forest/mountain/water/ocean/city without wall
  float defence_bonus_wall{4.0f};
  float veteran_hp_bonus{5.0f};
  int veteran_kills_required{3};

  // --- Economy (live-rules.md §3.3, §7) --- Both verified in the live client.
  int tech_cost_base{4};              // cost = tier * cities + 4
  int literacy_numerator{2};          // ceil(cost * 2/3), NOT the "33%" the wiki states
  int literacy_denominator{3};
  int city_income_per_level{1};
  int workshop_income{1};
  int park_income{1};
  int park_score{250};
  int capital_income_bonus_human{1};

  // --- Cities (live-rules.md §4) ---
  int city_base_score{100};
  int city_score_per_level{50};
  int population_per_score{5};
  // Initial reveal is 5x5 EXPLORED around the capital while only 3x3 is OWNED
  // (resolutions.md C1). An earlier owner answer said radius 1 and was wrong; the turn-0
  // score identity, which reproduces all 16 published scores, requires 25 explored tiles.
  int initial_vision_radius{2};

  // Settlement density, per LAND tile. The documented formula is (floor(w/3)^2 - capitals)
  // * 0.3, which is integer-quantised and ignores height: on an 11x11 it yields 2 villages
  // against 4 ruins, i.e. more ruins than villages on a small map, which is plainly wrong
  // and caps a duel at 2 possible opponents. Replaced with a smooth area-based density.
  // UNVERIFIED -- these are tunable data precisely so the real counts can be dialled in
  // without a rebuild once measured in game.
  float village_density{0.062f};
  float ruin_density{0.030f};
  int new_city_border{1};      // 3x3 = radius 1
  int grown_city_border{2};    // 5x5 = radius 2 (resolutions.md R3)
  int connection_population{1};  // +1 to BOTH ends (R23)

  // Population needed to reach level N from N-1. Index 0 unused.
  int population_threshold[16]{0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

  // --- Temples (live-rules.md §5.2, corrected by resolutions.md R24) ---
  // Owner-verified in game and CONTRADICTS every wiki source, which said 12 turns to max.
  int temple_growth_interval{2};
  int temple_max_level{5};
  int temple_score_per_level{100};
  int temple_build_population{1};

  // --- Turn structure (resolutions.md R1, R2, R25, R26) ---
  bool income_at_turn_start{true};
  bool fresh_units_cannot_act{true};
  bool capture_requires_turn_start{true};
  bool besieged_city_produces_nothing{true};

  // --- Movement (resolutions.md R19, R20) ---
  bool zoc_consumes_all_movement{true};
  int mountain_vision_bonus{1};

  // --- Difficulty: a CAPITAL income modifier, not a global economy bonus. ---
  // Normal bots have no handicap; they match a human exactly (live-rules.md §12.1).
  int difficulty_capital_bonus[static_cast<int>(Difficulty::Count)]{0, 1, 2, 4};

  UnitStats units[kUnitTypeCount]{};
  ImprovementStats improvements[static_cast<int>(Improvement::Count)]{};
  ResourceStats resources[static_cast<int>(Resource::Count)]{};
  TribeStats tribes[kTribeCount]{};

  // Rules whose authority is OWNER or ASSUMED rather than a cited source. Populated from
  // the JSON's "_unverified" array so the set of things we might be wrong about is
  // queryable at runtime rather than folklore in a doc.
  std::vector<std::string> unverified;

  [[nodiscard]] const UnitStats& unit(UnitType t) const {
    return units[static_cast<int>(t)];
  }
  [[nodiscard]] const ImprovementStats& improvement(Improvement i) const {
    return improvements[static_cast<int>(i)];
  }
  [[nodiscard]] const ResourceStats& resource(Resource r) const {
    return resources[static_cast<int>(r)];
  }
  [[nodiscard]] const TribeStats& tribe(Tribe t) const { return tribes[static_cast<int>(t)]; }

  // cost = tier * numCities + 4, with Literacy an exact one-third reduction.
  // Integer arithmetic throughout: at 111 tested data points the float form
  // ceil(cost * 0.67) fails 28 of them (resolutions.md R11).
  [[nodiscard]] int tech_cost(Tech t, int num_cities, bool has_literacy) const {
    const int base = tech_tier(t) * num_cities + tech_cost_base;
    if (!has_literacy) return base;
    return (base * literacy_numerator + literacy_denominator - 1) / literacy_denominator;
  }

  // The built-in fallback table. Mirrors live-rules.md; superseded by JSON.
  static Rules defaults();
};

// Loads data/rules/*.json over a defaults() base. Returns false and fills `error` on a
// malformed file. A file that parses but omits keys is fine -- omitted keys keep their
// default, which is what makes partial rule overlays (e.g. a tribes-2020 preset) possible.
bool load_rules(const std::string& path, Rules& out, std::string& error);

}  // namespace poly
