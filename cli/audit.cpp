// The bug hunter.
//
// WHY THIS EXISTS. Five defects were reported from playing a build whose 89 unit tests all
// passed: healing unavailable after moving, Escape dead in the move-first ordering, Persist
// never implemented, level-4 Population Growth unreachable, ruin rewards missing and their
// popup buried. Patching them one at a time was not converging, and the reason is that they
// share a shape the unit suite structurally cannot see.
//
// Every one of them is a REACHABILITY defect: an option the engine never offers, a skill it
// never consults, a menu entry that cannot appear, an outcome that cannot be rolled. A unit
// test sets up the state it wants and calls the thing directly, so it proves a mechanism works
// WHEN INVOKED and says nothing about whether the game ever invokes it. Worse, the natural
// fixture hides the bug: the starting Warrior has Dash, so it heals after moving; an attack
// from a standing start leaves moves_left intact, so Escape looks fine. Four of my five first
// attempts at a regression test passed against the broken engine.
//
// So this is the other half of the testing strategy, and it asks two questions the unit tests
// cannot:
//
//   1. INVARIANTS -- play a great many games, and after EVERY action check things that must be
//      true of any legal position. Chiefly: does every option the game offers actually work?
//      That single check is what turns "a button that does nothing" from a play report into a
//      failing build.
//
//   2. CENSUS -- count how often every verb, unit type, reward, improvement, tech and ruin
//      outcome is offered and taken across all of that play. Anything that never happens is
//      the interesting result. Persist, the level-4 reward and the water Rammer are all ZEROS
//      in a census; none of them is a failing assertion anywhere.
//
// A zero in the census is not automatically a bug -- some content is genuinely rare or needs a
// tech the random walk never buys. It is a QUESTION, and the point is that the questions are
// generated rather than waited for.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "poly/game.hpp"
#include "poly/mapgen.hpp"

#include "agents/agent.hpp"
#include "agents/registry.hpp"

namespace poly::audit {
namespace {

// ---------------------------------------------------------------------------------------
// Findings
// ---------------------------------------------------------------------------------------
// Every violation carries the seed and the step that produced it, because a fuzz finding you
// cannot replay is a rumour. `--audit-seed N` re-runs one game.
struct Finding {
  std::string check;
  std::string detail;
  std::uint64_t seed{0};
  int step{0};
};

class Report {
 public:
  void fail(const std::string& check, const std::string& detail, std::uint64_t seed, int step) {
    // One example per check is enough to act on, and stops a systemic break from producing
    // a hundred thousand identical lines.
    if (counts_[check]++ > 0) return;
    findings_.push_back(Finding{check, detail, seed, step});
  }
  [[nodiscard]] const std::vector<Finding>& findings() const { return findings_; }
  [[nodiscard]] int total(const std::string& check) const {
    const auto it = counts_.find(check);
    return it == counts_.end() ? 0 : it->second;
  }
  [[nodiscard]] bool empty() const { return findings_.empty(); }

 private:
  std::vector<Finding> findings_;
  std::map<std::string, int> counts_;
};

// ---------------------------------------------------------------------------------------
// Census
// ---------------------------------------------------------------------------------------
struct Census {
  std::map<std::string, long long> offered;
  std::map<std::string, long long> taken;
  // The largest honest player-turn seen. The turn/terminates threshold has to sit above this
  // or it reports busy turns as loops -- which is exactly what a guessed 400 did on a 16x16
  // late game, where a turn legitimately runs into the thousands of decisions.
  int max_turn_decisions{0};

  void offer(const std::string& key) { offered[key] += 0 + 1; }
  void take(const std::string& key) { taken[key] += 1; }
  // Registers a key with zero counts so it can be REPORTED as never seen. A census that only
  // lists what happened cannot tell you what did not.
  void expect(const std::string& key) {
    offered.emplace(key, 0);
    taken.emplace(key, 0);
  }
};

const char* verb_name(Verb v) {
  static const char* kNames[] = {"move",  "attack",  "capture", "harvest", "build",
                                 "train", "upgrade_naval", "research", "recover", "heal",
                                 "convert", "promote", "examine", "destroy", "disband",
                                 "choose_reward", "end_turn", "deselect"};
  const int i = static_cast<int>(v);
  return (i >= 0 && i < kVerbCount) ? kNames[i] : "?";
}

const char* unit_name(UnitType t) {
  static const char* kNames[] = {"warrior", "rider", "archer", "defender", "swordsman",
                                 "mind_bender", "catapult", "knight", "cloak", "dagger",
                                 "giant", "explorer", "raft", "scout", "rammer", "bomber",
                                 "juggernaut", "dinghy", "pirate"};
  const int i = static_cast<int>(t);
  return (i >= 0 && i < kUnitTypeCount) ? kNames[i] : "?";
}

const char* reward_name(CityReward r) {
  static const char* kNames[] = {"workshop", "explorer", "city_wall", "resources",
                                 "population_growth", "border_growth", "park", "super_unit"};
  const int i = static_cast<int>(r);
  return (i >= 0 && i < static_cast<int>(CityReward::Count)) ? kNames[i] : "?";
}

// ---------------------------------------------------------------------------------------
// Invariants over a single position
// ---------------------------------------------------------------------------------------
void check_state(const GameState& s, const Rules& rules, Report& report, std::uint64_t seed,
                 int step) {
  // The unit/tile relation is a bijection. Every desync in this engine's history -- a unit on
  // two tiles, a corpse still blocking a square, a Giant shoved into occupied ground -- shows
  // up here first, and it is cheap to check.
  for (UnitId u = 0; u < s.unit_count; ++u) {
    const Unit& unit = s.units[u];
    if (!unit.alive()) continue;
    if (unit.tile < 0 || unit.tile >= static_cast<TileIdx>(s.tile_count())) {
      report.fail("state/unit-tile-in-range",
                  "unit " + std::to_string(u) + " on tile " + std::to_string(unit.tile), seed,
                  step);
      continue;
    }
    if (s.tiles[unit.tile].unit != u) {
      report.fail("state/tile-unit-bijection",
                  "unit " + std::to_string(u) + " thinks it is on tile " +
                      std::to_string(unit.tile) + " which holds unit " +
                      std::to_string(s.tiles[unit.tile].unit),
                  seed, step);
    }
    // unit_max_hp() returns WHOLE hp; Unit::hp is in half-point units (kHpScale). Getting
    // this wrong made the audit's first run scream on every healthy unit -- a reminder that
    // an invariant checker is code and gets the same review as the code it checks.
    const int max_hp = to_hp(unit_max_hp(rules, unit));
    if (unit.hp > max_hp) {
      report.fail("state/unit-hp-within-max",
                  std::string(unit_name(unit.type)) + " hp " + std::to_string(unit.hp) +
                      " > max " + std::to_string(max_hp),
                  seed, step);
    }
    if (unit.owner < 0 || unit.owner >= s.player_count) {
      report.fail("state/unit-owner-valid", "unit " + std::to_string(u), seed, step);
    }
  }
  for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
    const UnitId u = s.tiles[t].unit;
    if (u == kNoUnit) continue;
    if (u < 0 || u >= s.unit_count || !s.units[u].alive() || s.units[u].tile != t) {
      report.fail("state/tile-unit-bijection",
                  "tile " + std::to_string(t) + " holds stale unit " + std::to_string(u), seed,
                  step);
    }
  }
  // DERIVED VALUES MUST MATCH THE BOARD.
  //
  // This is the class the first version of this audit could not see at all. It checked
  // REACHABILITY -- can you get to the option, does it apply, does it change something -- and
  // never once asked whether the resulting NUMBER was right. Windmill adjacency is pure
  // computation over the board, so it had exactly zero coverage, and the owner found it by
  // playing: a Windmill built before its Farms was worth nothing forever, because the yield
  // was banked at build time instead of being a function of the neighbourhood.
  //
  // The general form of the bug is "a derived value stored as an event". The general form of
  // the check is an ORACLE: recompute it independently and assert equality. That catches every
  // path that mutates the board without resyncing -- build, destroy, capture, border growth,
  // a ruin -- rather than the one path someone remembered to test.
  for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
    const int want = tile_population_yield(s, rules, t);
    if (s.tiles[t].population_contribution != want) {
      report.fail("derived/tile-population-matches-board",
                  "tile " + std::to_string(t) + " banked " +
                      std::to_string(s.tiles[t].population_contribution) + " population but " +
                      "the board says " + std::to_string(want),
                  seed, step);
    }
  }

  // EVERY CACHED DERIVED VALUE, checked by IDEMPOTENCE.
  //
  // Population was one instance of a general problem: the engine caches things it derives from
  // the board -- the Besieged flag, ConnectedToCapital, explored tiles -- and recomputes them
  // only at the handful of places someone remembered. Any other path that changes the board
  // leaves them stale, and stale caches are silent.
  //
  // Rather than reimplement each rule (and risk the oracle being the wrong one), this asks the
  // engine to recompute and asserts NOTHING CHANGES. That is the definition of a cache being
  // in sync, it needs no second implementation, and it covers every cached value at once --
  // including any added later.
  {
    GameState resynced = s;
    resync_derived_state(resynced, rules);
    for (CityId c = 0; c < s.city_count; ++c) {
      if (resynced.cities[c].has(CityFlag::Besieged) != s.cities[c].has(CityFlag::Besieged)) {
        report.fail("derived/siege-flag-in-sync",
                    "city " + std::to_string(c) + " Besieged=" +
                        std::to_string(s.cities[c].has(CityFlag::Besieged)) +
                        " but the board says " +
                        std::to_string(resynced.cities[c].has(CityFlag::Besieged)),
                    seed, step);
      }
      if (resynced.cities[c].has(CityFlag::ConnectedToCapital) !=
          s.cities[c].has(CityFlag::ConnectedToCapital)) {
        report.fail("derived/city-connection-in-sync",
                    "city " + std::to_string(c) + " ConnectedToCapital=" +
                        std::to_string(s.cities[c].has(CityFlag::ConnectedToCapital)) +
                        " but the board says " +
                        std::to_string(resynced.cities[c].has(CityFlag::ConnectedToCapital)),
                    seed, step);
      }
    }
    for (PlayerId p = 0; p < s.player_count; ++p) {
      for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
        if (resynced.explored[p].test(t) && !s.explored[p].test(t)) {
          report.fail("derived/visibility-in-sync",
                      "player " + std::to_string(p) + " should see tile " + std::to_string(t) +
                          " but it is still fogged",
                      seed, step);
          break;
        }
      }
    }
  }

  // Counts the engine keeps by hand, recomputed independently. These have no recompute
  // function to lean on, so they DO need a second implementation -- but they are simple enough
  // that the second implementation is obviously right.
  for (CityId c = 0; c < s.city_count; ++c) {
    if (s.cities[c].owner == kNoPlayer) continue;
    int actual = 0;
    for (UnitId u = 0; u < s.unit_count; ++u) {
      if (s.units[u].alive() && s.units[u].home_city == c) ++actual;
    }
    if (s.cities[c].unit_count != actual) {
      report.fail("derived/city-unit-count",
                  "city " + std::to_string(c) + " has banked unit_count " +
                      std::to_string(s.cities[c].unit_count) + " but owns " +
                      std::to_string(actual) + " living units -- this gates Train, so a drift " +
                      "here silently changes how many units a city may build",
                  seed, step);
    }
  }
  for (PlayerId p = 0; p < s.player_count; ++p) {
    bool has_city = false;
    for (CityId c = 0; c < s.city_count; ++c) {
      if (s.cities[c].owner == p) has_city = true;
    }
    if (s.players[p].alive && !has_city) {
      report.fail("derived/player-alive",
                  "player " + std::to_string(p) + " is alive with no cities", seed, step);
    }
  }

  for (PlayerId p = 0; p < s.player_count; ++p) {
    if (s.players[p].stars < 0) {
      report.fail("state/stars-nonnegative",
                  "player " + std::to_string(p) + " has " + std::to_string(s.players[p].stars),
                  seed, step);
    }
  }
  for (CityId c = 0; c < s.city_count; ++c) {
    const City& city = s.cities[c];
    if (city.pending_rewards < 0) {
      report.fail("state/pending-rewards-nonnegative", "city " + std::to_string(c), seed, step);
    }
    // R36: the reward menu is derived from level - pending + 1, so a count that outruns the
    // level would index a menu for a level the city never reached.
    if (city.owner != kNoPlayer && city.pending_rewards > city.level) {
      report.fail("state/pending-rewards-not-above-level",
                  "city " + std::to_string(c) + " level " + std::to_string(city.level) +
                      " owes " + std::to_string(city.pending_rewards),
                  seed, step);
    }
  }
}

// THE CENTRAL CHECK: everything the game offers, the game accepts.
//
// `enumerate_options(..., include_unavailable=false)` documents itself as yielding "exactly
// the actions apply() will accept". The human UI builds its menus from it. So any option that
// is offered and then refused is a button that does nothing -- which is precisely how the
// healing bug presented, and it is checkable without knowing anything about healing.
void check_offers_are_applicable(const GameState& s, const Rules& rules, Report& report,
                                 Census& census, std::uint64_t seed, int step) {
  if (s.phase != Phase::Select) return;  // menus are a SELECT-phase concept
  std::vector<Option> options;
  Layer layers[4];

  for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
    const int layer_count = tile_layers(s, t, layers);
    for (int i = 0; i < layer_count; ++i) {
      enumerate_options(s, rules, t, layers[i], false, options);
      for (const Option& o : options) {
        if (o.status != Availability::Available) continue;

        // Census, keyed finely enough to be actionable. A verb that never appears at all is a
        // different problem from a verb that never appears for one unit type.
        census.offer(std::string("verb/") + verb_name(o.verb));
        const UnitId actor = s.tiles[t].unit;
        if (actor != kNoUnit && layers[i] == Layer::Unit) {
          census.offer(std::string("unit-verb/") + unit_name(s.units[actor].type) + "." +
                       verb_name(o.verb));
        }
        if (o.verb == Verb::ChooseReward) {
          census.offer(std::string("reward/") + reward_name(o.reward));
        }
        if (o.verb == Verb::Train) {
          census.offer(std::string("train/") + unit_name(o.unit_type));
        }

        // The TARGET is not always carried on the option. Move and Attack are "now pick a
        // square" verbs -- the UI paints rings from reachable_tiles() and the attack range --
        // so their Option leaves `action` unset. Probing them with a default-constructed
        // action means probing tile 0, which is why the audit's first run reported ten
        // thousand refused Moves that were entirely its own fault.
        std::vector<int> targets;
        if (o.verb == Verb::Move) {
          ActionMask reach;
          reachable_tiles(s, rules, actor, reach);
          std::int16_t buf[kActionSlots];
          const int n = reach.collect(buf, kActionSlots);
          for (int k = 0; k < n; ++k) targets.push_back(buf[k]);
        } else if (o.verb == Verb::Convert) {
          // Like Move and Attack, Convert is a "pick a square" verb and carries no target on
          // the option. Mirrors legal_targets: any adjacent enemy.
          const Coord c = s.dims.coord(t);
          for (const Coord off : kNeighbourOffsets) {
            const Coord n{static_cast<std::int16_t>(c.x + off.x),
                          static_cast<std::int16_t>(c.y + off.y)};
            if (!s.dims.contains(n)) continue;
            const TileIdx e = s.dims.index(n);
            const UnitId other = s.tiles[e].unit;
            if (other != kNoUnit && s.units[other].alive() &&
                s.units[other].owner != s.units[actor].owner) {
              targets.push_back(tile_to_slot(e));
            }
          }
        } else if (o.verb == Verb::Attack) {
          for (TileIdx e = 0; e < static_cast<TileIdx>(s.tile_count()); ++e) {
            if (preview_combat(s, rules, t, e).valid) targets.push_back(tile_to_slot(e));
          }
        } else {
          targets.push_back(o.action.index);
        }
        if (targets.empty()) {
          report.fail("offer/has-a-target",
                      std::string("offered ") + verb_name(o.verb) + " on tile " +
                          std::to_string(t) + " but no target is legal for it",
                      seed, step);
          continue;
        }

        for (const int target : targets) {
          // Does it actually work? On a CLONE, so the audited game is unaffected.
          GameState probe = s;
          if (!apply_compound(probe, rules, t, o.verb, target, nullptr)) {
            report.fail("offer/applies",
                        std::string("offered ") + verb_name(o.verb) + " on tile " +
                            std::to_string(t) + " -> target " + std::to_string(target) +
                            " was refused by apply()",
                        seed, step);
            continue;
          }
          // And does it DO anything? A verb that succeeds and changes nothing is the other
          // half of the same complaint, and it is what makes a turn fail to terminate.
          if (o.verb != Verb::Deselect && state_hash(probe) == state_hash(s)) {
            report.fail("offer/changes-state",
                        std::string("offered ") + verb_name(o.verb) + " on tile " +
                            std::to_string(t) + " succeeded but changed nothing",
                        seed, step);
          }
        }
      }
    }
  }
}

// ORDER INDEPENDENCE. Two builds on different tiles are independent actions, so doing A then B
// must leave the same position as B then A.
//
// This is a METAMORPHIC test: it needs no oracle and no expected value, only the observation
// that the game should not care what order you did unrelated things in. It catches the windmill
// bug directly and without knowing anything about windmills -- build the mill then the farm,
// build the farm then the mill, compare. A check like this is worth more than any number of
// specific assertions, because it does not have to anticipate WHICH derived value is wrong.
void check_build_order_independence(const GameState& s, const Rules& rules, Report& report,
                                    std::uint64_t seed, int step) {
  if (s.phase != Phase::Select) return;
  std::vector<Option> options;
  struct Build { TileIdx tile; int slot; };
  std::vector<Build> builds;
  for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()) && builds.size() < 4; ++t) {
    enumerate_options(s, rules, t, Layer::Tile, false, options);
    for (const Option& o : options) {
      if (o.verb == Verb::Build && o.status == Availability::Available) {
        builds.push_back(Build{t, o.action.index});
        break;
      }
    }
  }
  for (std::size_t i = 0; i + 1 < builds.size(); ++i) {
    const Build& a = builds[i];
    const Build& b = builds[i + 1];
    if (a.tile == b.tile) continue;
    GameState ab = s, ba = s;
    if (!apply_compound(ab, rules, a.tile, Verb::Build, a.slot, nullptr)) continue;
    if (!apply_compound(ab, rules, b.tile, Verb::Build, b.slot, nullptr)) continue;
    if (!apply_compound(ba, rules, b.tile, Verb::Build, b.slot, nullptr)) continue;
    if (!apply_compound(ba, rules, a.tile, Verb::Build, a.slot, nullptr)) continue;
    // Compare what the buildings were FOR, not the whole state: the phase cursor legitimately
    // differs, and so does anything that consumed the state RNG.
    for (CityId c = 0; c < s.city_count; ++c) {
      if (ab.cities[c].population != ba.cities[c].population ||
          ab.cities[c].level != ba.cities[c].level) {
        report.fail("derived/build-order-independent",
                    "building tile " + std::to_string(a.tile) + " then " +
                        std::to_string(b.tile) + " gives city " + std::to_string(c) +
                        " population " + std::to_string(ab.cities[c].population) +
                        " but the other order gives " + std::to_string(ba.cities[c].population),
                    seed, step);
        return;
      }
    }
  }
}

// The architecture's own claim, from options.hpp: `legal_actions()` is a PROJECTION of
// `enumerate_options()`, "so the agent's view and the human's menu cannot drift". That is an
// assertion, and assertions in comments are wishes. This is the test.
void check_mask_matches_options(const GameState& s, const Rules& rules, Report& report,
                                std::uint64_t seed, int step) {
  if (s.phase != Phase::Verb || s.global_actor) return;
  ActionMask mask;
  legal_actions(s, rules, mask);

  std::vector<Option> options;
  Layer layers[4];
  bool from_options[kVerbCount] = {};
  const int layer_count = tile_layers(s, s.selected, layers);
  for (int i = 0; i < layer_count; ++i) {
    enumerate_options(s, rules, s.selected, layers[i], false, options);
    for (const Option& o : options) from_options[static_cast<int>(o.verb)] = true;
  }

  for (int v = 0; v < kVerbCount; ++v) {
    const auto verb = static_cast<Verb>(v);
    if (verb == Verb::Deselect || verb == Verb::EndTurn) continue;  // machine verbs, not menu
    const bool in_mask = mask.test(v);
    if (in_mask && !from_options[v]) {
      report.fail("projection/mask-has-verb-no-option",
                  std::string("mask offers ") + verb_name(verb) +
                      " but no option enumerates it (agent can do what the human cannot)"
                      " -- tile " + std::to_string(s.selected) + ", layers " +
                      std::to_string(layer_count) + ", city " +
                      std::to_string(city_at(s, s.selected)) + ", unit " +
                      std::to_string(s.tiles[s.selected].unit),
                  seed, step);
    }
    if (!in_mask && from_options[v]) {
      report.fail("projection/option-has-verb-no-mask",
                  std::string("option offers ") + verb_name(verb) +
                      " but the mask forbids it (human sees a button the engine refuses)"
                      " -- tile " + std::to_string(s.selected) + ", city " +
                      std::to_string(city_at(s, s.selected)) + ", unit " +
                      std::to_string(s.tiles[s.selected].unit),
                  seed, step);
    }
  }
}

// ---------------------------------------------------------------------------------------
// One audited game
// ---------------------------------------------------------------------------------------
struct GameSpec {
  int dim{16};
  int players{4};
  bool fog{true};
  std::uint64_t seed{1};
};

// Vary the shape of the game, because half the defects this session were only reachable in one
// configuration -- a non-Dash unit, a water tile, a city that levels twice.
//
// Game index -> configuration lives HERE, in one function, because run() and run_seed() have to
// agree. They did not: run_seed hardcoded 16x16/4/fog while run() cycled five shapes, so
// replaying a finding could silently set up a different game and report "no violations on this
// seed". The replay instruction printed with every finding was therefore not trustworthy --
// which defeats the purpose of recording a seed at all.
GameSpec spec_for_game(int g) {
  static const GameSpec kShapes[] = {
      {16, 4, true, 0}, {16, 2, false, 0}, {11, 2, false, 0}, {20, 4, true, 0}, {11, 4, true, 0},
  };
  const int count = static_cast<int>(sizeof(kShapes) / sizeof(kShapes[0]));
  GameSpec spec = kShapes[g % count];
  spec.seed = static_cast<std::uint64_t>(g + 1);
  return spec;
}

// Three games in four are played by the scripted agent, which actually reaches the content; the
// fourth is uniform random, which reaches the strange positions.
std::unique_ptr<agents::Agent> driver_for_game(int g, std::uint64_t seed) {
  if (g % 4 == 3) return nullptr;
  std::unique_ptr<agents::Agent> driver = agents::make_agent("rulebased");
  if (driver) driver->seed(seed * 2654435761u + 7u);
  return driver;
}

// `driver` picks the actions. Uniform-random play is the classic fuzzer and it is a poor
// EXPLORER of this game: it almost never walks a unit onto a ruin, rarely lands an attack, and
// so leaves most of the content unreached -- the first run reported every ruin reward as
// "never seen" purely because random play never examines one. Driving most games with the
// scripted agent gets the census somewhere near real play; keeping some random preserves the
// weird states a competent policy never visits.
void audit_game(const GameSpec& spec, const Rules& rules, Report& report, Census& census,
                int max_steps, bool deep, agents::Agent* driver) {
  GameConfig cfg;
  cfg.dims = MapDims{static_cast<std::int16_t>(spec.dim), static_cast<std::int16_t>(spec.dim)};
  cfg.seed = spec.seed;
  cfg.player_count = spec.players;
  cfg.fog = spec.fog;
  cfg.turn_limit = 60;
  for (int p = 0; p < spec.players; ++p) {
    cfg.tribes[p] = static_cast<Tribe>((static_cast<int>(spec.seed) + p) % kTribeCount);
  }

  GameState s;
  std::string error;
  if (!new_game(cfg, rules, s, error)) {
    report.fail("setup/new-game", error, spec.seed, 0);
    return;
  }

  int decisions_this_turn = 0;
  std::vector<std::string> recent;
  std::int32_t turn_at_last = s.turn;
  PlayerId mover_at_last = s.current_player;
  std::vector<std::int8_t> level_seen(static_cast<std::size_t>(kMaxCities), 0);

  for (int step = 0; step < max_steps; ++step) {
    if (outcome(s, rules).has_value()) break;

    check_state(s, rules, report, spec.seed, step);
    // The expensive checks clone a state per offered option, so they run on a subset of games.
    if (deep) {
      check_offers_are_applicable(s, rules, report, census, spec.seed, step);
      check_mask_matches_options(s, rules, report, spec.seed, step);
      check_build_order_independence(s, rules, report, spec.seed, step);
    }

    // City level is monotonic (R17) -- it must never be recomputed from population.
    for (CityId c = 0; c < s.city_count; ++c) {
      const auto ci = static_cast<std::size_t>(c);
      if (s.cities[c].level < level_seen[ci]) {
        report.fail("state/city-level-monotonic",
                    "city " + std::to_string(c) + " fell from " +
                        std::to_string(level_seen[ci]) + " to " +
                        std::to_string(s.cities[c].level),
                    spec.seed, step);
      }
      level_seen[ci] = s.cities[c].level;
    }

    if (s.turn != turn_at_last || s.current_player != mover_at_last) {
      decisions_this_turn = 0;
      turn_at_last = s.turn;
      mover_at_last = s.current_player;
    }
    // A turn that cannot end is the no-op cycle of HANDOFF section 4. 400 is far above any
    // honest turn; this fires on a loop, not on a busy turn.
    if (decisions_this_turn > census.max_turn_decisions) {
      census.max_turn_decisions = decisions_this_turn;
    }
    // MEASURED, not guessed. See Census::max_turn_decisions: a real turn on a large map late
    // in the game runs to a few thousand micro-decisions, because every unit, city and
    // buildable tile costs three. A loop is unbounded, so the threshold only has to be clear
    // of the honest maximum -- an order of magnitude above it, rather than just above it.
    if (++decisions_this_turn > 20000) {
      // Say WHAT it is looping on. "The turn did not end" is not actionable; the repeating
      // (phase, tile, verb) cycle is.
      std::string trail;
      for (const std::string& e : recent) trail += e + " ";
      report.fail("turn/terminates",
                  "player " + std::to_string(s.current_player) + " turn " +
                      std::to_string(s.turn) + " exceeded 400 decisions; last decisions: " +
                      trail,
                  spec.seed, step);
      break;
    }

    ActionMask mask;
    legal_actions(s, rules, mask);
    if (mask.empty()) {
      report.fail("legality/never-empty",
                  "no legal action in phase " + std::to_string(static_cast<int>(s.phase)),
                  spec.seed, step);
      break;
    }

    std::int16_t slots[kActionSlots];
    const int count = mask.collect(slots, kActionSlots);
    Action action{slots[static_cast<int>(s.rng.bounded(static_cast<std::uint32_t>(count)))]};
    if (driver != nullptr) {
      agents::ForwardModel model(rules, 2000);
      const Action chosen = driver->choose(s, model, mask, s.current_player);
      // A driver that returns something illegal is itself a finding -- the agent and the
      // legality mask are supposed to agree by construction.
      if (mask.test(chosen.index)) {
        action = chosen;
      } else {
        report.fail("agent/returns-legal-action",
                    std::string(driver->name()) + " returned illegal slot " +
                        std::to_string(chosen.index),
                    spec.seed, step);
      }
    }

    // Census of what actually gets DONE, which is the other half of "offered".
    if (s.phase == Phase::Verb) census.take(std::string("verb/") + verb_name(static_cast<Verb>(action.index)));
    if (s.phase == Phase::Target && s.pending_verb == Verb::ChooseReward &&
        slot_is_reward(action.index)) {
      census.take(std::string("reward/") + reward_name(slot_to_reward(action.index)));
    }
    if (s.phase == Phase::Target && s.pending_verb == Verb::Train && slot_is_train(action.index)) {
      census.take(std::string("train/") + unit_name(slot_to_unit_type(action.index)));
    }

    {
      std::string entry;
      if (s.phase == Phase::Select) {
        entry = "sel:" + std::to_string(action.index);
      } else if (s.phase == Phase::Verb) {
        entry = std::string("verb:") + verb_name(static_cast<Verb>(action.index));
      } else {
        entry = std::string("tgt<") + verb_name(s.pending_verb) + ">:" +
                std::to_string(action.index);
      }
      recent.push_back(entry);
      if (recent.size() > 12) recent.erase(recent.begin());
    }

    std::vector<std::string> log;
    if (!apply(s, rules, action, &log)) {
      report.fail("legality/mask-action-applies",
                  "action " + std::to_string(action.index) + " was in the legal mask and " +
                      "apply() refused it",
                  spec.seed, step);
      break;
    }
    // Ruin outcomes announce themselves; the census is how we learn one can never be rolled.
    for (const std::string& line : log) {
      if (line.rfind("Ruin: ", 0) != 0) continue;
      const std::size_t dash = line.find(" -- ");
      if (dash == std::string::npos) {
        report.fail("ruin/log-shape",
                    "ruin line does not match the 'Ruin: <title> -- <body>' the UI parses: " +
                        line,
                    spec.seed, step);
        continue;
      }
      census.take("ruin/" + line.substr(6, dash - 6));
    }
  }
}

// ---------------------------------------------------------------------------------------

void seed_expectations(const Rules& rules, Census& census) {
  // Everything the CONTENT says exists. A key that stays at zero after a long run is content
  // the game cannot produce -- which is exactly what "Persist is never implemented" and "a
  // water ruin cannot give a Rammer" look like from the outside.
  for (int v = 0; v < kVerbCount; ++v) census.expect(std::string("verb/") + verb_name(static_cast<Verb>(v)));
  for (int r = 0; r < static_cast<int>(CityReward::Count); ++r) {
    census.expect(std::string("reward/") + reward_name(static_cast<CityReward>(r)));
  }
  for (int u = 0; u < kUnitTypeCount; ++u) {
    const auto type = static_cast<UnitType>(u);
    if (rules.unit(type).trainable) census.expect(std::string("train/") + unit_name(type));
  }
  for (const char* r : {"Resources", "Scrolls of Wisdom", "Population", "New Friends", "Explorer"}) {
    census.expect(std::string("ruin/") + r);
  }
}

}  // namespace

int run(int games, const Rules& rules, bool verbose) {
  Report report;
  Census census;
  seed_expectations(rules, census);

  for (int g = 0; g < games; ++g) {
    GameSpec spec = spec_for_game(g);
    // The per-option probe clones a state for every offered option, which is far too slow to
    // run on every game. One in eight gets the deep treatment; the rest get the cheap
    // invariants at full speed, which is what makes the census broad.
    const bool deep = (g % 8) == 0;
    std::unique_ptr<agents::Agent> driver = driver_for_game(g, spec.seed);
    audit_game(spec, rules, report, census, 6000, deep, driver.get());
    if (verbose && (g + 1) % 25 == 0) {
      std::printf("  ... %d/%d games, %zu distinct findings\n", g + 1, games,
                  report.findings().size());
      std::fflush(stdout);
    }
  }

  std::printf("\n=== INVARIANT VIOLATIONS ===\n");
  if (report.empty()) {
    std::printf("  none\n");
  } else {
    for (const Finding& f : report.findings()) {
      std::printf("  [%s] x%d\n    %s\n    replay: --audit-seed %llu (step %d)\n", f.check.c_str(),
                  report.total(f.check), f.detail.c_str(),
                  static_cast<unsigned long long>(f.seed), f.step);
    }
  }

  std::printf("\n=== NEVER OFFERED / NEVER TAKEN ===\n");
  std::printf("A zero is a QUESTION, not a verdict: some content is legitimately rare, or\n");
  std::printf("needs a tech a random walk does not buy. Anything you cannot explain is a bug.\n\n");
  int zeros = 0;
  for (const auto& [key, count] : census.offered) {
    const auto taken = census.taken.find(key);
    const long long took = taken == census.taken.end() ? 0 : taken->second;
    if (count == 0 && took == 0) {
      std::printf("  NEVER   %s\n", key.c_str());
      ++zeros;
    }
  }
  for (const auto& [key, took] : census.taken) {
    if (took > 0) continue;
    if (census.offered.count(key) && census.offered.at(key) == 0) continue;  // already printed
    std::printf("  OFFERED BUT NEVER TAKEN   %s\n", key.c_str());
    ++zeros;
  }
  if (zeros == 0) std::printf("  none -- every catalogued behaviour occurred\n");

  if (verbose) {
    std::printf("\n=== CENSUS ===\n");
    for (const auto& [key, count] : census.offered) {
      const auto taken = census.taken.find(key);
      std::printf("  %-40s offered %-10lld taken %lld\n", key.c_str(), count,
                  taken == census.taken.end() ? 0 : taken->second);
    }
  }

  std::printf("\nlargest honest player-turn: %d decisions (loop threshold is 20000)\n",
              census.max_turn_decisions);
  std::printf("\n%zu invariant violations, %d never-seen behaviours, over %d games.\n",
              report.findings().size(), zeros, games);
  return report.empty() ? 0 : 1;
}

// Replays ONE game with every check on, so a finding can be reproduced in isolation.
int run_seed(std::uint64_t seed, const Rules& rules) {
  Report report;
  Census census;
  seed_expectations(rules, census);
  // Reconstruct the EXACT game the sweep ran: seed = index + 1, so the index is seed - 1, and
  // that index picks both the map shape and the driver.
  const int g = static_cast<int>(seed) - 1;
  const GameSpec spec = spec_for_game(g);
  std::unique_ptr<agents::Agent> driver = driver_for_game(g, spec.seed);
  std::printf("replaying seed %llu: %dx%d, %d players, fog %s, driver %s\n",
              static_cast<unsigned long long>(seed), spec.dim, spec.dim, spec.players,
              spec.fog ? "on" : "off", driver ? driver->name() : "random");
  audit_game(spec, rules, report, census, 6000, true, driver.get());
  for (const Finding& f : report.findings()) {
    std::printf("  [%s] step %d\n    %s\n", f.check.c_str(), f.step, f.detail.c_str());
  }
  if (report.empty()) std::printf("  no violations on this seed\n");
  return report.empty() ? 0 : 1;
}

}  // namespace poly::audit
