// Content enums: what exists in the game.
//
// MEMBERSHIP ONLY. Every number (cost, HP, attack, population yield) lives in
// data/rules/live.json, per docs/phase0-plan.md §3 — structure in code, parameters in data.
//
// Every enum here is sourced from docs/rules-research/live-rules.md against live v2.16.3.
// Nothing is from memory. Section references are to that file.
//
// Scope is the 12 regular tribes (README §2). Members belonging only to Aquarion, Elyrion,
// Polaris or Cymanti are absent, not commented out.

#pragma once

#include <cstdint>

#include "poly/types.hpp"

namespace poly {

// ---------------------------------------------------------------------------------------
// Resources (live-rules.md §6.2)
// ---------------------------------------------------------------------------------------
// Whale is deliberately absent: REMOVED from the live game and replaced by Starfish.
// Sources written before Patch of the Ocean still list it.
enum class Resource : std::uint8_t {
  None = 0,
  Fruit,       // Field. Organization (T1).
  Crop,        // Field. Revealed by Organization, harvested by building a Farm.
  Game,        // Forest. Hunting (T1).
  Fish,        // Water/Ocean. Fishing (T1).
  Starfish,    // Water/Ocean. Navigation (T3). Stars, not population.
  Metal,       // Mountain. Revealed by Climbing, harvested by building a Mine.
  Ruin,        // Any terrain except shallow water.
  Village,     // Land. Becomes a level-1 city when captured.
  Lighthouse,  // The four map corners only.
  Count,
};

// ---------------------------------------------------------------------------------------
// Tile improvements (live-rules.md §5.1, §5.2)
// ---------------------------------------------------------------------------------------
// Workshop, City Wall and Park are NOT here — they are city-level upgrade rewards and live
// as flags on City, not as tile occupants. Roads and bridges are tile flags for the same
// reason: a tile may hold a road AND an improvement.
enum class Improvement : std::uint8_t {
  None = 0,
  LumberHut,      // Forest.  Forestry (T2).
  Sawmill,        // Field.   Mathematics (T3). 1/city. Output scales with adjacent Lumber Huts.
  Farm,           // Field+Crop. Farming (T2).
  Windmill,       // Field.   Construction (T3). 1/city. Scales with adjacent Farms.
  Mine,           // Mountain+Metal. Mining (T2).
  Forge,          // Field OR Forest. Smithery (T3). 1/city. Scales with adjacent Mines.
  Port,           // Shallow water. Fishing (T1) -- NOT Sailing; two wiki pages are stale.
  Market,         // Field. Trade (T3). 1/city (owner-verified). Income = sum of adjacent
                  // Sawmill/Windmill/Forge LEVELS (resolutions.md R21).
  Temple,         // Field. Free Spirit (T3).
  ForestTemple,   // Forest.
  MountainTemple, // Mountain.
  WaterTemple,    // Water or Ocean.
  // Roads and bridges are TILE FLAGS in the state (a tile may carry a road AND a building),
  // but they need action-space slots to be buildable, so they appear here as targets only.
  Road,
  Bridge,
  Count,
};

// ---------------------------------------------------------------------------------------
// Units (live-rules.md §2.1, §2.2)
// ---------------------------------------------------------------------------------------
enum class UnitType : std::uint8_t {
  // Land, trainable
  Warrior = 0,
  Rider,
  Archer,
  Defender,
  Swordsman,
  MindBender,
  Catapult,
  Knight,
  Cloak,

  // Land, not trainable
  Dagger,    // Spawned by a Cloak's Infiltrate.
  Giant,     // Super unit. City level 5+ reward. The super unit for ALL 12 regular tribes.
  Explorer,  // Auto-moving fog-clearer from the level-2 city reward. 12 moves (v2.16.3).

  // Naval. The Path of the Ocean rework (2.8): Boat/Ship/Battleship and the Float skill no
  // longer exist. A naval unit carries NO HP of its own -- it is the same entity as the
  // embarked land unit, and damage carries both ways (resolutions.md R29).
  Raft,
  Scout,
  Rammer,
  Bomber,
  Juggernaut,  // Naval super unit. Has Stomp.
  Dinghy,      // A Cloak that embarked. Loses Creep. Not upgradeable.
  Pirate,      // A Dagger that embarked, or spawned on water. Reverts to Dagger on landing.

  Count,
};

inline constexpr int kUnitTypeCount = static_cast<int>(UnitType::Count);

// ---------------------------------------------------------------------------------------
// Technology (live-rules.md §3.1)
// ---------------------------------------------------------------------------------------
// 25 techs in a perfectly regular shape: 5 branches x (1 T1 + 2 T2 + 2 T3).
//
// Declaration order encodes the tree, so tier and parent are computed rather than tabulated
// (see tech_tier / tech_parent below). A hand-written parent table would be 25 more numbers
// to get wrong; the regularity is real, so it is worth exploiting.
//
// Per branch: [T1, T2a, T3a, T2b, T3b] -- each T3 immediately follows its T2 parent.
//
// Whaling and Engineering are absent: both REMOVED from the live game.
enum class Tech : std::uint8_t {
  // Climbing branch
  Climbing = 0,
  Mining, Smithery,
  Meditation, Philosophy,

  // Fishing branch
  Fishing,
  Sailing, Navigation,
  Ramming, Aquatism,

  // Hunting branch
  Hunting,
  Archery, Spiritualism,
  Forestry, Mathematics,

  // Organization branch
  Organization,
  Farming, Construction,
  Strategy, Diplomacy,

  // Riding branch
  Riding,
  Roads, Trade,
  FreeSpirit, Chivalry,

  Count,
};

inline constexpr int kTechCount = static_cast<int>(Tech::Count);
inline constexpr int kTechBranches = 5;
inline constexpr int kTechsPerBranch = 5;

static_assert(kTechCount == kTechBranches * kTechsPerBranch,
              "The tech tree is 5 branches of 5. tech_tier and tech_parent depend on it.");

// Position within a branch: 0 = T1 root, 1/3 = T2, 2/4 = T3.
constexpr int tech_slot(Tech t) { return static_cast<int>(t) % kTechsPerBranch; }
constexpr int tech_branch(Tech t) { return static_cast<int>(t) / kTechsPerBranch; }

// Tier is 1, 2 or 3 -- this is the `tier` in cost = tier * numCities + 4 (§3.3).
constexpr int tech_tier(Tech t) {
  switch (tech_slot(t)) {
    case 0: return 1;
    case 1: case 3: return 2;
    default: return 3;
  }
}

// The parent a tech is researched from. The T1 root of each branch has none.
//
// Backwards research (resolutions.md R30) means the edge is traversable in BOTH directions:
// owning a child makes its parent researchable. Because the tree is only three deep, owning
// a T3 transitively opens its whole branch upward -- which is why no source ever needed to
// disambiguate "direct parent" from "whole branch".
constexpr Tech tech_parent(Tech t) {
  const int base = tech_branch(t) * kTechsPerBranch;
  switch (tech_slot(t)) {
    case 1: case 3: return static_cast<Tech>(base);          // T2 -> branch root
    case 2: return static_cast<Tech>(base + 1);              // T3a -> T2a
    case 4: return static_cast<Tech>(base + 3);              // T3b -> T2b
    default: return t;                                        // T1 root: no parent
  }
}

constexpr bool tech_is_root(Tech t) { return tech_slot(t) == 0; }

// ---------------------------------------------------------------------------------------
// City upgrade rewards (live-rules.md §4.3)
// ---------------------------------------------------------------------------------------
// Levels 5 and above repeat the Park / SuperUnit choice indefinitely.
enum class CityReward : std::uint8_t {
  Workshop = 0,    // L2 option A. +1 star/turn.
  Explorer,        // L2 option B.
  CityWall,        // L3 option A. Raises defenceBonus 1.5 -> 4 for Fortify units.
  Resources,       // L3 option B. +5 stars, one time.
  PopulationGrowth,// L4 option A. +3 population.
  BorderGrowth,    // L4 option B. Expands to 5x5, never over claimed tiles (R3).
  Park,            // L5+ option A. +250 score AND +1 star/turn.
  SuperUnit,       // L5+ option B. Spawns a Giant; displaces any unit on the city (R27).
  Count,
};

// ---------------------------------------------------------------------------------------
// Unit skills (live-rules.md §2.3)
// ---------------------------------------------------------------------------------------
// Held as a bitmask on UnitType in the rules data.
//
// Dash and Escape are ACTION-SPACE modifiers, not stat modifiers (resolutions.md R26):
// they change which verbs remain legal after another verb resolves. The phase machine reads
// them directly.
enum class Skill : std::uint32_t {
  None = 0,
  Dash = 1u << 0,        // May move and then attack.
  Escape = 1u << 1,      // May attack and then move.
  Fortify = 1u << 2,     // Eligible for the city / city-wall defence bonus.
  Persist = 1u << 3,     // Attacks again after a kill (Knight chaining).
  Heal = 1u << 4,
  Convert = 1u << 5,
  Scout = 1u << 6,       // Extended vision radius.
  Hide = 1u << 7,
  Infiltrate = 1u << 8,  // Cloak -> spawns Daggers in an enemy city.
  Creep = 1u << 9,       // Ignores terrain movement cost. Does NOT negate ZoC (default, R5).
  Stiff = 1u << 10,      // No retaliation.
  Static = 1u << 11,     // Cannot be promoted to veteran.
  Surprise = 1u << 12,
  Independent = 1u << 13,
  Stomp = 1u << 14,      // Juggernaut. Damage magnitude UNRESOLVED #2.
  Splash = 1u << 15,     // Bomber.
  Carry = 1u << 16,      // Port embark.
  Navigate = 1u << 17,   // May enter deep ocean.
};

constexpr Skill operator|(Skill a, Skill b) {
  return static_cast<Skill>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

constexpr bool has_skill(Skill set, Skill query) {
  return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(query)) != 0;
}

// ---------------------------------------------------------------------------------------
// Tribes (live-rules.md §10.1)
// ---------------------------------------------------------------------------------------
// The 12 regular tribes. Aquarion, Elyrion, Polaris and Cymanti are out of scope (README §2)
// -- excluded for surface area, not for any structural reason.
enum class Tribe : std::uint8_t {
  XinXi = 0,
  Imperius,
  Bardur,
  Oumaji,
  Kickoo,
  Hoodrick,
  Luxidoor,   // Capital starts at level 3 (resolutions.md R12).
  Vengir,
  Zebasi,
  AiMo,
  Quetzali,
  Yadakk,
  Count,
};

inline constexpr int kTribeCount = static_cast<int>(Tribe::Count);
static_assert(kTribeCount == 12, "12 regular tribes; specials are out of scope.");

// ---------------------------------------------------------------------------------------
// Game modes (live-rules.md §11.1)
// ---------------------------------------------------------------------------------------
// Modelled as data (a VictoryCondition config) rather than an if-ladder, so that "any game
// mode" is a configuration rather than a code change -- see docs/phase0-plan.md §4.
enum class GameMode : std::uint8_t {
  Domination = 0,  // "Might". The competitive 1v1 format.
  Perfection,      // Highest score at a fixed turn limit.
  Glory,
  Creative,        // Sandbox: no victory condition.
  Count,
};

// ---------------------------------------------------------------------------------------
// Difficulty (live-rules.md §12.1)
// ---------------------------------------------------------------------------------------
// The handicap is a CAPITAL income modifier, not a global economy bonus, and Normal bots
// have no handicap at all -- they match a human exactly. Dev-confirmed (Zoythrus, 2020).
// Level-1 capital income: Easy 1, Human 2, Normal 2, Hard 3, Crazy 5.
enum class Difficulty : std::uint8_t {
  Easy = 0,
  Normal,
  Hard,
  Crazy,
  Count,
};

}  // namespace poly
