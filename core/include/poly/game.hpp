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
#include "poly/options.hpp"
#include "poly/rules.hpp"
#include "poly/state.hpp"

namespace poly {

struct GameConfig {
  // The base game's default setup: 16x16 with four tribes. The 11x11 duel is the COMPETITIVE
  // format and remains what the agent is ultimately trained for, but it is not what a player
  // sees when they press New Game, and testing against the wrong default hides whole classes
  // of problem (village density, multi-front war, naval relevance).
  //
  // This comment was already here and already right, and MatchConfig disagreed with it anyway
  // for as long as the two remembered the numbers independently. The value now comes from one
  // named constant, so a default that is correct here cannot be wrong there.
  MapDims dims{kDefaultMapDims};
  std::uint64_t seed{1};
  GameMode mode{GameMode::Domination};
  int turn_limit{0};  // 0 = none
  bool fog{true};
  float land_ratio{0.6f};

  int player_count{kDefaultPlayerCount};
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
// `precomputed` is the legality mask for THIS state, when the caller already has it. Passing
// it skips recomputation -- it does not skip validation, so legality remains the engine's job.
bool apply(GameState& s, const Rules& rules, Action action, std::vector<std::string>* log,
           const ActionMask* precomputed = nullptr);

// nullopt while the game is ongoing. See outcome.hpp for why this is not a float.
MaybeOutcome outcome(const GameState& s, const Rules& rules);

// --- queries used by the UI, the bot, and later the evaluation function -----------------

int city_income(const GameState& s, const Rules& rules, CityId city);
int player_income(const GameState& s, const Rules& rules, PlayerId player);
int player_score(const GameState& s, const Rules& rules, PlayerId player);
int population_needed(const GameState& s, const Rules& rules, CityId city);

// What a tile SHOULD contribute to its city's population, computed from the board alone.
// `Tile::population_contribution` is the engine's running copy of this; any divergence means
// something changed the board without resyncing, and the audit asserts they agree.
int tile_population_yield(const GameState& s, const Rules& rules, TileIdx tile);

// Recomputes every CACHED DERIVED value from the board: siege flags, city connections,
// visibility, and per-tile population yields.
//
// This exists for the AUDIT, not as a safety net, and the distinction matters. Calling it
// after every action would paper over exactly the bugs it is meant to expose. Its job is to be
// IDEMPOTENT: running it on a legal position must change nothing, and any difference means
// some path mutated the board without resyncing. That is a general oracle for cached state,
// and it needs no second implementation that could itself be wrong.
void resync_derived_state(GameState& s, const Rules& rules);

// Test seam for capture, which is otherwise only reachable through a unit standing on a city
// at the start of its turn -- too much choreography for a test about population bookkeeping.
void capture_city_for_test(GameState& s, const Rules& rules, CityId city, PlayerId new_owner,
                           std::vector<std::string>* log);

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

// A unit's maximum HP in WHOLE points, including the veteran +5 and, for an embarked unit,
// the HP of the land unit inside the hull rather than the hull's own. The StateView must
// report this rather than a raw type stat: promotion raises the maximum, so a bare
// rules.unit(type).hp lookup renders a freshly promoted unit as 15/10.
int unit_max_hp(const Rules& rules, const Unit& u);

bool unit_can_act(const GameState& s, UnitId unit);
CityId city_at(const GameState& s, TileIdx tile);
void recompute_visibility(GameState& s, const Rules& rules, PlayerId player);

// Marks newly-encountered tribes. Meeting is an event that reveals identity and pays stars
// scaled to the met tribe's score (resolutions.md C3), so it must run after visibility.
void check_meetings(GameState& s, const Rules& rules, PlayerId player,
                    std::vector<std::string>* log);

// Serialises a StateView per docs/wire-protocol.md. `viewer` = kNoPlayer for full
// observability. Fog is applied HERE, server-side: unexplored tiles are omitted entirely,
// so a UI bug cannot leak the map.
// True while the current player still has ANY available action anywhere -- units, cities,
// tiles or research. Drives when End Turn lights up.
bool has_any_available_action(const GameState& s, const Rules& rules);

// The city with an unresolved level-up, or kNoCity. Must be answered immediately.
CityId pending_reward_city(const GameState& s);

void enumerate_research(const GameState& s, const Rules& rules, bool include_unavailable,
                        std::vector<Option>& out);

// Drives the phase machine through one complete human-scale action (select -> verb -> target).
// The UI thinks in "build a Farm here"; the agent thinks in micro-decisions. Both route
// through apply(), so there is one implementation of what an action does. Rolls back on any
// failure. Pass tile = kNoTile for the global actor (research, end turn).
bool apply_compound(GameState& s, const Rules& rules, TileIdx tile, Verb verb, int target_slot,
                    std::vector<std::string>* log);

std::string state_view_json(const GameState& s, const Rules& rules, PlayerId viewer);

}  // namespace poly
