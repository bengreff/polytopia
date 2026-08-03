// The engine's entire public surface.
//
// Three functions matter: legal_actions() says what may be done, apply() does one of them,
// and outcome() says whether the game is over. Everything else is a query.
//
// Invariants this API enforces structurally (docs/phase0-plan.md §1):
//   - Legality is the engine's job. Nothing outside core/ constructs an action and hopes.
//   - outcome() returns MaybeOutcome. A horizon cutoff is nullopt, exactly like any other
//     unfinished position, so it can never be confused with a real terminal result.
//   - Perspective is always explicit. Nothing infers whose value a position holds from
//     search depth -- Polytopia allows consecutive same-player moves.

#pragma once

#include <string>
#include <vector>

#include "poly/action.hpp"
#include "poly/outcome.hpp"
#include "poly/rules.hpp"
#include "poly/state.hpp"

namespace poly {

struct GameConfig {
  MapDims dims{11, 11};
  std::uint64_t seed{1};
  GameMode mode{GameMode::Domination};
  int turn_limit{0};  // 0 = none
  bool fog{true};
  float land_ratio{0.6f};

  int player_count{2};
  Tribe tribes[kMaxPlayers]{};
  bool is_human[kMaxPlayers]{};
  Difficulty difficulty[kMaxPlayers]{};
};

// Creates a new game. Returns false and fills `error` on an unusable config.
bool new_game(const GameConfig& cfg, const Rules& rules, GameState& out, std::string& error);

// Fills `mask` with every legal action for the CURRENT decision, given the state's phase.
// Never empty for an ongoing game: EndTurn or Deselect is always available, so a player can
// never be stuck with nothing to do.
void legal_actions(const GameState& s, const Rules& rules, ActionMask& mask);

// Applies one action. Returns false if it was not legal, leaving the state untouched.
// `log` receives human-readable lines for the UI's action feed; pass nullptr to skip.
bool apply(GameState& s, const Rules& rules, Action action, std::vector<std::string>* log);

// nullopt while the game is ongoing. See outcome.hpp for why this is not a float.
MaybeOutcome outcome(const GameState& s, const Rules& rules);

// --- queries used by the UI, the bot, and later the evaluation function -----------------

int city_income(const GameState& s, const Rules& rules, CityId city);
int player_income(const GameState& s, const Rules& rules, PlayerId player);
int player_score(const GameState& s, const Rules& rules, PlayerId player);
int population_needed(const GameState& s, const Rules& rules, CityId city);

// Damage a would deal to b, and b's retaliation, in half-point HP units.
struct CombatResult {
  int attacker_damage{0};   // dealt BY the attacker TO the defender
  int defender_damage{0};   // retaliation
  bool defender_dies{false};
  bool retaliates{false};
};
CombatResult resolve_combat(const GameState& s, const Rules& rules, UnitId attacker,
                            UnitId defender);

// Tiles a unit may legally move to this turn. Movement is destination-only: the player picks
// a destination and the engine routes (resolutions.md R19), so this is the whole move API.
void reachable_tiles(const GameState& s, const Rules& rules, UnitId unit, ActionMask& out);

// Credits income, resets units, and recomputes visibility for the current player. Called on
// every turn change AND once by new_game -- without that call the opening player would begin
// the game with no income at all, since nothing has changed hands yet.
void begin_turn(GameState& s, const Rules& rules);

bool unit_can_act(const GameState& s, UnitId unit);
CityId city_at(const GameState& s, TileIdx tile);
void recompute_visibility(GameState& s, const Rules& rules, PlayerId player);

// Serialises a StateView per docs/wire-protocol.md. `viewer` = kNoPlayer for full
// observability. Fog is applied HERE, server-side: unexplored tiles are omitted entirely,
// so a UI bug cannot leak the map.
std::string state_view_json(const GameState& s, const Rules& rules, PlayerId viewer);

}  // namespace poly
