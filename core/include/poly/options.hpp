// What can happen on a tile, and why the things that cannot, cannot.
//
// This exists because a legality mask can only ever say NO. By the time an action is illegal
// it has already been deleted from the set, so the mask structurally cannot express
// "Farm -- 5 stars, you have 3" or "Mine -- needs Mining". Wiring the human UI to the mask
// produced an interface where most tiles were unclickable and actions silently vanished.
//
// The real game does the opposite, and its own shipped API proves it:
// `CommandUtils.GetTrainableUnits(..., bool includeUnavailable)` -- and the UI passes TRUE.
// Every button carries a second "unavailable" callback whose only job is to explain the
// refusal. See docs/ui-reference.md §2.
//
// THE INVARIANT: the predicates below are the single source of truth. legal_actions() is a
// PROJECTION of them (status == Available), not a parallel implementation. If the agent's
// legality and the human's menu could be computed separately they would drift, and
// README.md §4 names that as the failure the project cannot survive.

#pragma once

#include <cstdint>
#include <vector>

#include "poly/action.hpp"
#include "poly/content.hpp"
#include "poly/types.hpp"

namespace poly {

// Forward declared rather than included: options.hpp is pulled in by game.hpp ahead of
// state.hpp and rules.hpp, and these are only ever used by reference here.
struct GameState;
struct Rules;

enum class Availability : std::uint8_t {
  // Do it. `Option::action` is valid and will be accepted by apply().
  Available = 0,
  // Right shape, not enough stars. The price stays legible; the commit button goes dark red.
  // NOT hidden -- knowing what you are saving toward is most of the game's economy.
  CannotAfford,
  // Needs research. Renders "(locked)" with no commit button, and offers a TECH TREE jump.
  // There is no padlock icon anywhere in this game (ui-reference.md §2).
  MissingTech,
  // Structurally impossible right now. Annotated in place: a full city still LISTS the unit,
  // suffixed "(too many)".
  Blocked,
};

enum class BlockReason : std::uint8_t {
  None = 0,
  CityFull,          // unit_count >= capacity
  TileOccupied,      // something already stands here
  WrongTerrain,      // e.g. Farm off a field
  NoResource,        // e.g. Mine without Metal
  AlreadyBuilt,      // an improvement already occupies the tile
  PerCityLimit,      // one Sawmill/Windmill/Forge/Market per city
  NotYourTerritory,
  UnitSpent,         // already acted this turn
  FreshlyTrained,    // bought this turn; cannot act until next (R2)
  NeedsTurnStart,    // Capture and Examine require having STARTED the turn here (R25)
  OutOfRange,
  PendingReward,     // a level-up must be resolved before anything else
};

// Which layer of a tile an option belongs to. Tapping cycles unit -> city.
// There is deliberately no Tile layer on a city tile: you cannot build under your own city,
// and the owner confirmed the real game offers no such layer.
enum class Layer : std::uint8_t {
  Unit = 0,
  City,
  Tile,
  Count,
};

struct Option {
  Action action{};                     // valid only when status == Available
  Verb verb{Verb::Move};
  Layer layer{Layer::Tile};

  // What this option produces, when the verb needs a typed target. Exactly one is meaningful.
  UnitType unit_type{UnitType::Warrior};
  Improvement improvement{Improvement::None};
  Tech tech{Tech::Climbing};
  CityReward reward{CityReward::Workshop};

  int cost{0};                         // ALWAYS populated, even when unaffordable
  Availability status{Availability::Available};
  Tech required_tech{Tech::Climbing};  // meaningful when MissingTech
  BlockReason reason{BlockReason::None};
};

// Every option on a tile, for a given layer. With include_unavailable = false this yields
// exactly the actions apply() will accept.
//
// `viewer` gates nothing here -- this is the acting player's own view. Knowledge filtering
// happens at serialisation, not in the rules.
void enumerate_options(const GameState& s, const Rules& rules, TileIdx tile, Layer layer,
                       bool include_unavailable, std::vector<Option>& out);

// Which layers a tile actually has, in tap-cycle order.
int tile_layers(const GameState& s, TileIdx tile, Layer* out);

// Two-way damage preview, for the hover readout. The real game shows exact numbers for both
// sides plus a death warning for each (ui-reference.md §4), so this must be exposed rather
// than recomputed client-side -- a client cannot see defence bonuses it has no tech for.
struct CombatPreview {
  bool valid{false};
  int attacker_damage{0};  // whole HP dealt to the defender
  int defender_damage{0};  // retaliation
  bool defender_dies{false};
  bool attacker_dies{false};
};
CombatPreview preview_combat(const GameState& s, const Rules& rules, TileIdx from, TileIdx to);

}  // namespace poly
