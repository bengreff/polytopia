// What happened, as DATA rather than as English.
//
// `apply()` used to take a `std::vector<std::string>*` and push prose into it: "Ruin: Resources
// -- +10 stars", "Attack for 5, retaliation 3". Two things were wrong with that.
//
// The engine was writing the UI's copy. Wording is a presentation decision -- language,
// phrasing, how much detail to show -- and it was baked into the rules layer, where changing it
// means editing game.cpp and rebuilding the engine.
//
// Worse, the browser PARSED it back. `parseRuinLine` in hud.js regex-matches
// `/^(.*?)\s*--\s*(.*)$/` against the engine's sentence to recover the two fields it wanted.
// So the real contract between engine and UI was an English sentence with a double hyphen in
// it, discoverable only by reading both sides, and breakable by improving the wording.
//
// An Event carries the fields. Rendering lives in wire/, where a presentation change is a
// presentation change. The audit also benefits: asserting `kind == EventKind::RuinReward` is a
// real check, where matching a prose regex only tested that I had written the regex twice.

#pragma once

#include <cstdint>
#include <vector>

#include "poly/content.hpp"
#include "poly/types.hpp"

namespace poly {

enum class EventKind : std::uint8_t {
  None = 0,
  CityLevelUp,       // city, value = new level
  CityCaptured,      // city, player = new owner
  VillageCaptured,   // city
  UnitTrained,       // unit, city
  UnitDisbanded,     // unit, value = stars refunded
  UnitPromoted,      // unit
  UnitConverted,     // unit
  UnitHealed,        // unit, value = HP restored
  UnitsHealed,       // unit (the healer), value = how many
  NavalUpgrade,      // unit, unit_type = the hull it became
  Attacked,          // unit (attacker), target_unit, value = damage, value2 = retaliation
  SuperUnitArrived,  // unit, city
  OccupantShoved,    // unit
  OccupantCrushed,   // unit
  Researched,        // tech
  BuiltImprovement,  // tile, improvement
  BuiltRoad,         // tile
  BuiltBridge,       // tile
  DestroyedBuilding, // tile, improvement (what was there)
  Harvested,         // tile, resource
  ExplorerRevealed,  // value = tiles revealed
  RuinReward,        // ruin_reward, plus whichever payload field that reward carries
  TribeMet,          // player, value = stars paid
  Count,
};

// Which of the six ruin outcomes was rolled. The UI needs this to title its popup; it used to
// recover it by splitting a sentence on " -- ".
enum class RuinReward : std::uint8_t {
  None = 0,
  Resources,       // value = stars
  ScrollsOfWisdom, // tech
  Population,      // city, value = population added
  NewFriends,      // unit, unit_type = Swordsman on land, Rammer at sea
  Explorer,        // value = tiles revealed
  Count,
};

// One thing that happened. Deliberately a flat POD: events are produced in the hot path, and a
// variant or a string would put allocation there. Which fields are meaningful depends on
// `kind`, and the enum above documents that per case.
struct Event {
  EventKind kind{EventKind::None};
  RuinReward ruin_reward{RuinReward::None};

  PlayerId player{kNoPlayer};
  CityId city{kNoCity};
  UnitId unit{kNoUnit};
  UnitId target_unit{kNoUnit};
  TileIdx tile{kNoTile};

  UnitType unit_type{UnitType::Warrior};
  Improvement improvement{Improvement::None};
  Resource resource{Resource::None};
  Tech tech{Tech::Climbing};

  std::int32_t value{0};
  std::int32_t value2{0};

  friend constexpr bool operator==(const Event&, const Event&) = default;
};

// A sink the engine appends to. `nullptr` everywhere means "nobody is listening", which is the
// case for search: an agent stepping a forward model wants none of this, and paying for it was
// the other reason not to build strings here.
using EventLog = std::vector<Event>;

}  // namespace poly
