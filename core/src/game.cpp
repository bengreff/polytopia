#include "poly/game.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>

#include "poly/json.hpp"
#include "poly/mapgen.hpp"
#include "poly/options.hpp"

namespace poly {
namespace {

// --- small queries ----------------------------------------------------------------------

bool is_water(Terrain t) { return t == Terrain::Water || t == Terrain::Ocean; }

// Forest and Mountain "stop movement": a unit entering one ends its move there. A road
// negates this (live-rules.md §6.1).
bool terrain_stops_movement(const Tile& tile) {
  if (tile.has(TileFlag::Road) || tile.has(TileFlag::Bridge)) return false;
  return tile.terrain == Terrain::Forest || tile.terrain == Terrain::Mountain;
}

bool tribe_can_enter(const GameState& s, const Rules& rules, PlayerId player, const Tile& tile) {
  const Player& p = s.players[player];
  switch (tile.terrain) {
    case Terrain::Mountain: return p.has_tech(Tech::Climbing);
    // Shallow water needs a hull, which Fishing provides via a Port; deep ocean additionally
    // needs Sailing. A bridge makes the tile walkable outright.
    case Terrain::Water: return p.has_tech(Tech::Fishing) || tile.has(TileFlag::Bridge);
    case Terrain::Ocean: return p.has_tech(Tech::Sailing);
    default: return true;
  }
  (void)rules;
}

// Max HP in HALF-POINT units, which is what Unit::hp is stored in. Veterans get +5 whole HP.
int max_hp_for(const Rules& rules, const Unit& u) {
  int hp = rules.unit(u.type).hp;
  if (u.has(UnitFlag::Veteran)) hp += static_cast<int>(rules.veteran_hp_bonus);
  return to_hp(hp);
}

}  // namespace

// Declared in game.hpp: the StateView serialiser needs it too, so it cannot stay file-local.
int unit_max_hp(const Rules& rules, const Unit& u) {
  const UnitStats& stats = rules.unit(u.has(UnitFlag::Embarked) ? u.carried_type : u.type);
  int hp = stats.hp;
  if (u.has(UnitFlag::Veteran)) hp += static_cast<int>(rules.veteran_hp_bonus);
  return hp;
}

namespace {

// Is `tile` adjacent to a unit hostile to `player`? Drives Zone of Control.
bool adjacent_to_enemy(const GameState& s, TileIdx tile, PlayerId player) {
  const Coord c = s.dims.coord(tile);
  for (const Coord off : kNeighbourOffsets) {
    const Coord n{static_cast<std::int16_t>(c.x + off.x), static_cast<std::int16_t>(c.y + off.y)};
    if (!s.dims.contains(n)) continue;
    const UnitId u = s.tiles[s.dims.index(n)].unit;
    if (u != kNoUnit && s.units[u].owner != player && s.units[u].alive()) return true;
  }
  return false;
}

int count_adjacent_improvements(const GameState& s, TileIdx tile, Improvement want,
                                PlayerId owner) {
  const Coord c = s.dims.coord(tile);
  int total = 0;
  for (const Coord off : kNeighbourOffsets) {
    const Coord n{static_cast<std::int16_t>(c.x + off.x), static_cast<std::int16_t>(c.y + off.y)};
    if (!s.dims.contains(n)) continue;
    const Tile& t = s.tiles[s.dims.index(n)];
    if (t.improvement == want && t.owner == owner) ++total;
  }
  return total;
}

void add_population(GameState& s, const Rules& rules, CityId city_id, int amount,
                    std::vector<std::string>* log);

// What this tile contributes to its city's population, computed FROM THE BOARD.
//
// This is the oracle for the adjacency buildings. `Verb::Build` used to compute the yield once
// and hand it to the city, which made it an event rather than a value: a Windmill built before
// its Farms counted zero forever, and a Farm built beside an existing Windmill fed it nothing.
// Anything derived from neighbours has to be recomputed when the neighbourhood changes.
int tile_population_yield_impl(const GameState& s, const Rules& rules, TileIdx tile) {
  const Tile& t = s.tiles[tile];
  if (t.improvement == Improvement::None || t.owner == kNoPlayer) return 0;
  const ImprovementStats& stats = rules.improvement(t.improvement);
  if (stats.adjacency_source == Improvement::None) return stats.population;
  return stats.adjacency_population *
         count_adjacent_improvements(s, tile, stats.adjacency_source, t.owner);
}

// Recomputes `tile` and its eight neighbours, moving any difference into the owning city.
// Called after ANY change to an improvement, so build order cannot matter.
void resync_population_around(GameState& s, const Rules& rules, TileIdx tile,
                              std::vector<std::string>* log) {
  const Coord c = s.dims.coord(tile);
  TileIdx affected[1 + kNeighbourCount];
  int count = 0;
  affected[count++] = tile;
  for (const Coord off : kNeighbourOffsets) {
    const Coord n{static_cast<std::int16_t>(c.x + off.x), static_cast<std::int16_t>(c.y + off.y)};
    if (s.dims.contains(n)) affected[count++] = s.dims.index(n);
  }
  for (int i = 0; i < count; ++i) {
    Tile& t = s.tiles[affected[i]];
    const int want = tile_population_yield_impl(s, rules, affected[i]);
    const int delta = want - t.population_contribution;
    if (delta == 0) continue;
    t.population_contribution = static_cast<std::int8_t>(want);
    if (t.city != kNoCity) add_population(s, rules, t.city, delta, log);
  }
}

// Recomputes EVERY tile. For rare, sweeping changes -- a capture re-parents a whole city's
// territory, border growth claims a ring -- where working out the affected patch is more
// error-prone than just doing the board. A capture happens a few dozen times a game; 256 tiles
// is nothing, and the audit's oracle found this exact gap by noticing a tile that had banked 3
// population while the board said 2.
void resync_population_board(GameState& s, const Rules& rules, std::vector<std::string>* log) {
  for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
    Tile& tile = s.tiles[t];
    const int want = tile_population_yield_impl(s, rules, t);
    const int delta = want - tile.population_contribution;
    if (delta == 0) continue;
    tile.population_contribution = static_cast<std::int8_t>(want);
    if (tile.city != kNoCity) add_population(s, rules, tile.city, delta, log);
  }
}

// --- rounding ---------------------------------------------------------------------------

// Round half AWAY FROM ZERO (resolutions.md R10). Derived from a documented Warrior-vs-
// Warrior outcome landing on exactly 4.5 -> observed 5. This explicitly rules out banker's
// rounding, which is what std::nearbyint and C# Math.Round do by DEFAULT -- the trap this
// helper exists to avoid. All damage is non-negative, so floor(x + 0.5) is sufficient.
int round_half_up(float x) { return static_cast<int>(std::floor(x + 0.5f)); }

}  // namespace

// -----------------------------------------------------------------------------------------
// Queries
// -----------------------------------------------------------------------------------------

CityId city_at(const GameState& s, TileIdx tile) {
  for (CityId c = 0; c < s.city_count; ++c) {
    if (s.cities[c].tile == tile) return c;
  }
  return kNoCity;
}

// May this unit heal right now?
//
// Deliberately NOT gated on UnitFlag::TurnEnded. Moving sets that flag for every unit without
// Dash -- Defender, Catapult, Mind Bender, Giant -- so gating Recover on it meant "healing
// costs a whole turn standing still", and walking a wounded Defender back behind the line and
// then healing it was impossible. That is the ordinary defensive move (R33, owner-reported
// from play).
//
// What it IS gated on: having attacked (the attack is the unit's action for the turn), being
// freshly trained, and having already healed -- the last needing its own flag precisely
// because TurnEnded is no longer load-bearing here.
// Which level's reward menu is owed next.
//
// A city banks a COUNT of outstanding level-ups, not a list of levels, and both menus used to
// read `c.level` directly. So a city that crossed two thresholds at once -- a fat harvest, a
// capture, or Population Growth itself pushing it over -- answered every outstanding choice
// with the menu for its CURRENT level, and the lower level's reward could never be taken.
// That is why "Population Growth at level 4 does not work": a city that reached 5 in the same
// step was offered Park / Super Unit twice.
//
// No new state needed: with `pending` outstanding and the city now at `level`, those rewards
// were earned at levels level-pending+1 .. level, and the next to resolve is the first.
int reward_menu_level(const City& c) {
  const int earned_at = c.level - c.pending_rewards + 1;
  return std::clamp(earned_at, 2, 15);
}

bool may_recover(const GameState& s, const Rules& rules, UnitId unit) {
  if (unit == kNoUnit) return false;
  const Unit& u = s.units[unit];
  if (!u.alive() || u.owner != s.current_player) return false;
  if (u.has(UnitFlag::FreshlyTrained) || u.has(UnitFlag::HasAttacked)) return false;
  if (u.has(UnitFlag::HasRecovered)) return false;
  // A city owing a level-up freezes its whole tile: `legal_verbs` answers {ChooseReward,
  // Deselect} and nothing else. Recover deliberately ignores TurnEnded, which is what let it
  // slip past that freeze as well -- the audit caught it as an offered verb the mask forbade.
  const CityId here = city_at(s, u.tile);
  if (here != kNoCity && s.cities[here].owner == u.owner && s.cities[here].pending_rewards > 0) {
    return false;
  }
  return u.hp < max_hp_for(rules, u);
}

bool unit_can_act(const GameState& s, UnitId unit) {
  if (unit == kNoUnit) return false;
  const Unit& u = s.units[unit];
  if (!u.alive() || u.owner != s.current_player) return false;
  if (u.has(UnitFlag::TurnEnded)) return false;
  // A unit bought this turn cannot act (resolutions.md R2).
  if (u.has(UnitFlag::FreshlyTrained)) return false;
  return true;
}

int population_needed(const GameState& s, const Rules& rules, CityId city) {
  const City& c = s.cities[city];
  const int level = std::clamp<int>(c.level, 1, 15);
  return rules.population_threshold[level];
}

int city_income(const GameState& s, const Rules& rules, CityId city) {
  const City& c = s.cities[city];
  if (c.owner == kNoPlayer) return 0;
  // A besieged city produces nothing (resolutions.md R4, owner-flagged uncertain).
  if (rules.besieged_city_produces_nothing && c.has(CityFlag::Besieged)) return 0;

  int income = c.level * rules.city_income_per_level;
  if (c.has(CityFlag::Workshop)) income += rules.workshop_income;
  income += c.park_count * rules.park_income;

  if (c.has(CityFlag::Capital)) {
    const Player& p = s.players[c.owner];
    // The difficulty handicap is a CAPITAL modifier, not a global economy bonus, and Normal
    // bots have no handicap at all -- they match a human exactly (live-rules.md §12.1).
    income += p.is_human ? rules.capital_income_bonus_human
                         : rules.difficulty_capital_bonus[static_cast<int>(p.difficulty)];
  }

  // Each NEGATIVE population point costs one star of income. Population goes negative when
  // a captured city loses its trade network (resolutions.md R17).
  if (c.population < 0) income -= -c.population;

  // "A city cannot produce negative income."
  return std::max(0, income);
}

int player_income(const GameState& s, const Rules& rules, PlayerId player) {
  int total = 0;
  for (CityId c = 0; c < s.city_count; ++c) {
    if (s.cities[c].owner == player) total += city_income(s, rules, c);
  }
  // Market income is the SUM OF LEVELS of adjacent Sawmills/Windmills/Forges, one Market per
  // city (resolutions.md R21). PolyEnv holds two incompatible Market models and the DEAD one
  // was the correct one; this follows the owner-verified rule, not PolyEnv's live code.
  for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
    const Tile& tile = s.tiles[t];
    if (tile.improvement != Improvement::Market || tile.owner != player) continue;
    total += count_adjacent_improvements(s, t, Improvement::Sawmill, player);
    total += count_adjacent_improvements(s, t, Improvement::Windmill, player);
    total += count_adjacent_improvements(s, t, Improvement::Forge, player);
  }
  return total;
}

int player_score(const GameState& s, const Rules& rules, PlayerId player) {
  int score = 0;
  for (CityId c = 0; c < s.city_count; ++c) {
    const City& city = s.cities[c];
    if (city.owner != player) continue;
    score += rules.city_base_score + (city.level - 1) * rules.city_score_per_level;
    score += std::max(0, static_cast<int>(city.population)) * rules.population_per_score;
    score += city.park_count * rules.park_score;
  }
  // Research is 100 points per TIER, not per tech (live-rules.md §3.6).
  for (int i = 0; i < kTechCount; ++i) {
    const auto tech = static_cast<Tech>(i);
    if (s.players[player].has_tech(tech)) score += 100 * tech_tier(tech);
  }
  for (UnitId u = 0; u < s.unit_count; ++u) {
    const Unit& unit = s.units[u];
    if (unit.owner != player || !unit.alive()) continue;
    // 5 points per star of cost; super units are worth 50 (live-rules.md §11).
    score += 5 * rules.unit(unit.type).cost;
  }
  return score;
}

// -----------------------------------------------------------------------------------------
// Combat
// -----------------------------------------------------------------------------------------

CombatResult resolve_combat(const GameState& s, const Rules& rules, UnitId attacker_id,
                            UnitId defender_id) {
  CombatResult result;
  const Unit& a = s.units[attacker_id];
  const Unit& d = s.units[defender_id];

  const UnitStats& as = rules.unit(a.type);
  const UnitStats& ds = rules.unit(d.type);

  const float a_hp = static_cast<float>(a.hp) / static_cast<float>(kHpScale);
  const float d_hp = static_cast<float>(d.hp) / static_cast<float>(kHpScale);
  const float a_max = static_cast<float>(unit_max_hp(rules, a));
  const float d_max = static_cast<float>(unit_max_hp(rules, d));

  // Defence bonus is a SINGLE scalar selected by situation -- it does not stack
  // (live-rules.md §1.5).
  float bonus = rules.defence_bonus_none;
  const Tile& d_tile = s.tiles[d.tile];
  const CityId city = city_at(s, d.tile);
  if (city != kNoCity && s.cities[city].owner == d.owner) {
    // Cities grant their bonus ONLY to units with Fortify. A Giant never gets one.
    if (has_skill(static_cast<Skill>(ds.skills), Skill::Fortify)) {
      bonus = s.cities[city].has(CityFlag::Walled) ? rules.defence_bonus_wall
                                                   : rules.defence_bonus_standard;
    }
  } else {
    // Terrain bonuses require the corresponding technology.
    const Player& dp = s.players[d.owner];
    const bool forest = d_tile.terrain == Terrain::Forest && dp.has_tech(Tech::Archery);
    const bool mountain = d_tile.terrain == Terrain::Mountain && dp.has_tech(Tech::Climbing);
    const bool water = is_water(d_tile.terrain) && dp.has_tech(Tech::Aquatism);
    if (forest || mountain || water) bonus = rules.defence_bonus_standard;
  }

  // The attacker's own tile bonus does nothing -- there is no bonus term in attackForce.
  const float attack_force = as.attack * (a_hp / a_max);
  const float defence_force = ds.defence * (d_hp / d_max) * bonus;
  const float total = attack_force + defence_force;

  if (total <= 0.0f) {
    // 0 attack against 0 defence divides 0/0. Clamp rather than emit NaN.
    return result;
  }

  result.attacker_damage =
      round_half_up((attack_force / total) * as.attack * rules.accelerator) * kHpScale;
  result.defender_damage =
      round_half_up((defence_force / total) * ds.defence * rules.accelerator) * kHpScale;

  result.defender_dies = result.attacker_damage >= d.hp;

  // No retaliation if the defender dies, is out of range, or has Stiff. Both results are
  // computed from the PRE-combat state, so the defender's HP loss does not weaken its own
  // retaliation (live-rules.md §1.6).
  const bool stiff = has_skill(static_cast<Skill>(ds.skills), Skill::Stiff);
  const Coord ac = s.dims.coord(a.tile);
  const Coord dc = s.dims.coord(d.tile);
  const int chebyshev = std::max(std::abs(ac.x - dc.x), std::abs(ac.y - dc.y));
  const bool in_range = chebyshev <= ds.range;

  result.retaliates = !result.defender_dies && !stiff && in_range && result.defender_damage > 0;
  if (!result.retaliates) result.defender_damage = 0;
  return result;
}

// -----------------------------------------------------------------------------------------
// Movement
// -----------------------------------------------------------------------------------------

void reachable_tiles(const GameState& s, const Rules& rules, UnitId unit_id, ActionMask& out) {
  out.clear();
  if (unit_id == kNoUnit) return;
  const Unit& u = s.units[unit_id];
  if (u.moves_left <= 0) return;
  // A unit that has already acted, or was bought this turn, has no reachable tiles -- not
  // "tiles it may not use". Returning them made the UI paint movement rings that apply()
  // then refused, so the two disagreed about the same unit.
  if (u.has(UnitFlag::TurnEnded) || u.has(UnitFlag::FreshlyTrained)) return;
  // Attacking ends movement unless the unit has Escape (resolutions.md R26).
  if (u.has(UnitFlag::HasAttacked) &&
      !has_skill(static_cast<Skill>(rules.unit(u.type).skills), Skill::Escape)) {
    return;
  }

  // Movement is destination-only; the engine routes (resolutions.md R19). So this is a
  // bounded BFS over move points rather than a path enumeration -- which is exactly why the
  // action space needs no path encoding and the ~186 head holds.
  std::vector<int> cost(static_cast<std::size_t>(s.tile_count()), -1);
  std::queue<TileIdx> frontier;
  cost[static_cast<std::size_t>(u.tile)] = 0;
  frontier.push(u.tile);

  while (!frontier.empty()) {
    const TileIdx current = frontier.front();
    frontier.pop();
    const int spent = cost[static_cast<std::size_t>(current)];
    if (spent >= u.moves_left) continue;

    // Zone of Control: entering a tile adjacent to an enemy consumes ALL remaining movement
    // (resolutions.md R20), so such a tile is a dead end -- reachable, but not passed through.
    if (current != u.tile && rules.zoc_consumes_all_movement &&
        adjacent_to_enemy(s, current, u.owner)) {
      continue;
    }
    // Forest and Mountain stop movement on entry.
    if (current != u.tile && terrain_stops_movement(s.tiles[current])) continue;

    const Coord c = s.dims.coord(current);
    for (const Coord off : kNeighbourOffsets) {
      const Coord n{static_cast<std::int16_t>(c.x + off.x),
                    static_cast<std::int16_t>(c.y + off.y)};
      if (!s.dims.contains(n)) continue;
      const TileIdx next = s.dims.index(n);
      const Tile& tile = s.tiles[next];

      if (!tribe_can_enter(s, rules, u.owner, tile)) continue;
      // Units cannot move THROUGH enemy units, and cannot end on any occupied tile.
      if (tile.unit != kNoUnit) continue;

      const int next_cost = spent + 1;
      if (next_cost > u.moves_left) continue;
      if (cost[static_cast<std::size_t>(next)] >= 0 &&
          cost[static_cast<std::size_t>(next)] <= next_cost) {
        continue;
      }
      cost[static_cast<std::size_t>(next)] = next_cost;
      out.set(tile_to_slot(next));
      frontier.push(next);
    }
  }
}

// -----------------------------------------------------------------------------------------
// Visibility
// -----------------------------------------------------------------------------------------

void recompute_visibility(GameState& s, const Rules& rules, PlayerId player) {
  // Revealed tiles NEVER re-fog (resolutions.md R18), so this only ever ADDS. There is no
  // remembered-stale-state to age out, which is why one bitset suffices.
  ExploredMap& seen = s.explored[player];

  auto reveal_around = [&](TileIdx centre, int radius) {
    const Coord c = s.dims.coord(centre);
    for (int dy = -radius; dy <= radius; ++dy) {
      for (int dx = -radius; dx <= radius; ++dx) {
        const Coord n{static_cast<std::int16_t>(c.x + dx), static_cast<std::int16_t>(c.y + dy)};
        if (s.dims.contains(n)) seen.set(s.dims.index(n));
      }
    }
  };

  for (UnitId u = 0; u < s.unit_count; ++u) {
    const Unit& unit = s.units[u];
    if (unit.owner != player || !unit.alive()) continue;
    int radius = rules.unit(unit.type).vision;
    // THE SCOUT SKILL. "Units with the scout skill can see two tiles in every direction"
    // (wiki, Combat/Vision). It was declared in content.hpp, carried by the Scout, Cloak and
    // Dinghy in the rules data, and consulted NOWHERE -- so the Scout, a unit whose entire
    // purpose is vision, saw exactly as far as a Warrior. Found by tools/content_audit.py,
    // which is the same shape of defect as Persist and would never have surfaced from play:
    // "the Scout revealed no more than anything else" does not look like a bug from inside a
    // fogged game.
    if (has_skill(static_cast<Skill>(rules.unit(unit.type).skills), Skill::Scout)) {
      radius = std::max(radius, rules.scout_vision_radius);
    }
    // Standing on a mountain grants +1 vision (resolutions.md R18).
    if (s.tiles[unit.tile].terrain == Terrain::Mountain) radius += rules.mountain_vision_bonus;
    reveal_around(unit.tile, radius);
  }
  // Territory reveals fog.
  for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
    if (s.tiles[t].owner == player) seen.set(t);
  }
}

void check_meetings(GameState& s, const Rules& rules, PlayerId player,
                    std::vector<std::string>* log) {
  // Meeting a tribe is an EVENT, not a passive flag (resolutions.md C3): first sight of
  // another tribe's unit or city reveals their identity AND pays stars scaled to their
  // score. Modelling it as a flag would silently drop an economic mechanic.
  const ExploredMap& seen = s.explored[player];
  for (UnitId u = 0; u < s.unit_count; ++u) {
    const Unit& unit = s.units[u];
    if (!unit.alive() || unit.owner == player || unit.owner == kNoPlayer) continue;
    if (seen.test(unit.tile)) s.players[player].mark_met(unit.owner);
  }
  for (CityId c = 0; c < s.city_count; ++c) {
    const City& city = s.cities[c];
    if (city.owner == player || city.owner == kNoPlayer) continue;
    if (seen.test(city.tile)) s.players[player].mark_met(city.owner);
  }
  // Diplomacy reveals every tribe's identity without needing to find them.
  if (s.players[player].has_tech(Tech::Diplomacy)) {
    for (PlayerId p = 0; p < s.player_count; ++p) s.players[player].mark_met(p);
  }
  (void)rules;
  (void)log;
}

// -----------------------------------------------------------------------------------------
// City growth
// -----------------------------------------------------------------------------------------

namespace {

void add_population(GameState& s, const Rules& rules, CityId city_id, int amount,
                    std::vector<std::string>* log) {
  City& c = s.cities[city_id];
  c.population = static_cast<std::int16_t>(c.population + amount);

  // Surplus carries over (resolutions.md R15) -- population is a running total, not a bar
  // that resets. Level is MONOTONIC and never recomputed from population (R17): a captured
  // city driven negative keeps its level and shows a deficit instead.
  while (c.population >= population_needed(s, rules, city_id) && c.level < 15) {
    c.population = static_cast<std::int16_t>(c.population - population_needed(s, rules, city_id));
    c.level = static_cast<std::int8_t>(c.level + 1);
    c.pending_rewards = static_cast<std::int8_t>(c.pending_rewards + 1);
    if (log) log->push_back("City reached level " + std::to_string(c.level));
  }
}

void claim_territory(GameState& s, CityId city_id, int radius) {
  const City& c = s.cities[city_id];
  const Coord centre = s.dims.coord(c.tile);
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      const Coord n{static_cast<std::int16_t>(centre.x + dx),
                    static_cast<std::int16_t>(centre.y + dy)};
      if (!s.dims.contains(n)) continue;
      Tile& tile = s.tiles[s.dims.index(n)];
      // Never claim an already-owned tile (resolutions.md R3). This is why borders are
      // ragged tile sets rather than rectangles.
      if (tile.owner != kNoPlayer) continue;
      tile.owner = c.owner;
      tile.city = city_id;
    }
  }
}

void claim_territory_and_resync(GameState& s, const Rules& rules, CityId city_id, int radius,
                                std::vector<std::string>* log) {
  claim_territory(s, city_id, radius);
  resync_population_board(s, rules, log);
}

// A city is connected when a road/port network links it to the capital. Each connection gives
// +1 population to BOTH ends (resolutions.md R23), which is why capturing a capital is
// devastating: it collapses the whole network at once and drives the captured capital's
// population negative rather than dropping its level.
void recompute_city_connections(GameState& s, const Rules& rules, PlayerId player) {
  CityId capital = kNoCity;
  for (CityId c = 0; c < s.city_count; ++c) {
    if (s.cities[c].owner == player && s.cities[c].has(CityFlag::Capital)) capital = c;
  }
  // Clear first: a severed connection must actually go away, or capture would never cost
  // the loser anything.
  int previously = 0;
  for (CityId c = 0; c < s.city_count; ++c) {
    if (s.cities[c].owner != player) continue;
    if (s.cities[c].has(CityFlag::ConnectedToCapital)) ++previously;
    s.cities[c].remove(CityFlag::ConnectedToCapital);
  }
  if (capital == kNoCity) return;

  // Flood the road network from the capital. Roads and ports carry the link; a tile owned by
  // an enemy never does -- sea routes may cross NEUTRAL water but not enemy territory (R22).
  std::vector<bool> seen(static_cast<std::size_t>(s.tile_count()), false);
  std::queue<TileIdx> frontier;
  frontier.push(s.cities[capital].tile);
  seen[static_cast<std::size_t>(s.cities[capital].tile)] = true;
  int connected = 0;

  while (!frontier.empty()) {
    const TileIdx at = frontier.front();
    frontier.pop();
    const CityId here = city_at(s, at);
    if (here != kNoCity && s.cities[here].owner == player && here != capital) {
      s.cities[here].add(CityFlag::ConnectedToCapital);
      ++connected;
    }
    const Coord c = s.dims.coord(at);
    for (const Coord off : kNeighbourOffsets) {
      const Coord n{static_cast<std::int16_t>(c.x + off.x),
                    static_cast<std::int16_t>(c.y + off.y)};
      if (!s.dims.contains(n)) continue;
      const TileIdx t = s.dims.index(n);
      if (seen[static_cast<std::size_t>(t)]) continue;
      const Tile& tile = s.tiles[t];
      if (tile.owner != kNoPlayer && tile.owner != player) continue;  // never through enemies
      const bool carries = tile.has(TileFlag::Road) || tile.has(TileFlag::Bridge) ||
                           tile.improvement == Improvement::Port || city_at(s, t) != kNoCity;
      if (!carries) continue;
      seen[static_cast<std::size_t>(t)] = true;
      frontier.push(t);
    }
  }

  // Net change in connections adjusts the capital, each end being worth +1 (R23).
  if (connected != previously) {
    s.cities[capital].population = static_cast<std::int16_t>(
        s.cities[capital].population + (connected - previously) * rules.connection_population);
  }
  (void)rules;
}

void update_siege_flags(GameState& s) {
  for (CityId c = 0; c < s.city_count; ++c) {
    City& city = s.cities[c];
    const UnitId occupant = s.tiles[city.tile].unit;
    const bool besieged =
        occupant != kNoUnit && s.units[occupant].alive() && s.units[occupant].owner != city.owner;
    if (besieged) {
      city.add(CityFlag::Besieged);
    } else {
      city.remove(CityFlag::Besieged);
    }
  }
}

}  // namespace

// -----------------------------------------------------------------------------------------
// Turn cycle
// -----------------------------------------------------------------------------------------

void begin_turn(GameState& s, const Rules& rules) {
  const PlayerId player = s.current_player;
  update_siege_flags(s);

  // Income at the START of the turn (resolutions.md R1), so a city captured last turn is
  // spendable this turn -- EXCEPT on a player's very first turn, where the tribe's starting
  // stars already are the opening position. Crediting income there made Imperius open on 7
  // rather than its actual 5.
  if (rules.income_at_turn_start && s.players[player].has_started) {
    s.players[player].stars += player_income(s, rules, player);
  }
  s.players[player].has_started = true;

  for (UnitId u = 0; u < s.unit_count; ++u) {
    Unit& unit = s.units[u];
    if (unit.owner != player || !unit.alive()) continue;
    unit.moves_left = static_cast<std::uint8_t>(rules.unit(unit.type).move);
    unit.remove(UnitFlag::HasMoved);
    unit.remove(UnitFlag::HasAttacked);
    unit.remove(UnitFlag::TurnEnded);
    unit.remove(UnitFlag::ArrivedThisTurn);
    unit.remove(UnitFlag::HasRecovered);
    // Clears here, not on spawn: the unit becomes usable on the owner's NEXT turn.
    unit.remove(UnitFlag::FreshlyTrained);
  }

  // Temples grow on their own every 2 turns to level 5, scoring 100 per level
  // (resolutions.md R24 -- owner-verified, and contradicting every wiki source, which said
  // 12 turns to max rather than 8).
  if (rules.temple_growth_interval > 0 && s.turn % rules.temple_growth_interval == 0) {
    for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
      Tile& tile = s.tiles[t];
      if (tile.owner != player) continue;
      const bool is_temple = tile.improvement == Improvement::Temple ||
                             tile.improvement == Improvement::ForestTemple ||
                             tile.improvement == Improvement::MountainTemple ||
                             tile.improvement == Improvement::WaterTemple;
      if (!is_temple) continue;
      if (tile.improvement_level < rules.temple_max_level) {
        tile.improvement_level = static_cast<std::uint8_t>(tile.improvement_level + 1);
      }
    }
  }

  // Auto-recover: "If a unit does not take any action in a given turn, it will automatically
  // heal if it can do so." Applied at the START of the owner's next turn, because that is the
  // first moment we know the unit did nothing on the previous one.
  for (UnitId u = 0; u < s.unit_count; ++u) {
    Unit& unit = s.units[u];
    if (unit.owner != player || !unit.alive()) continue;
    if (unit.moves_used == 0 && unit.hp < max_hp_for(rules, unit)) {
      const int amount = (s.tiles[unit.tile].owner == player) ? 4 : 2;
      unit.hp = static_cast<std::int16_t>(
          std::min(max_hp_for(rules, unit), unit.hp + amount * kHpScale));
    }
    unit.moves_used = 0;
  }

  recompute_city_connections(s, rules, player);

  recompute_visibility(s, rules, player);
  check_meetings(s, rules, player, nullptr);
  s.phase = Phase::Select;
  s.selected = kNoTile;
  s.global_actor = false;
}

namespace {

void advance_player(GameState& s, const Rules& rules) {
  for (int i = 0; i < s.player_count; ++i) {
    s.current_player = static_cast<PlayerId>((s.current_player + 1) % s.player_count);
    if (s.players[s.current_player].alive) break;
  }
  if (s.current_player == 0) ++s.turn;
  begin_turn(s, rules);
}

}  // namespace

// -----------------------------------------------------------------------------------------
// Legality
// -----------------------------------------------------------------------------------------

namespace {

Availability can_train(const GameState& s, const Rules& rules, CityId city_id, UnitType type,
                       BlockReason& reason) {
  reason = BlockReason::None;
  const City& c = s.cities[city_id];
  const UnitStats& stats = rules.unit(type);
  if (!stats.trainable) {
    reason = BlockReason::WrongTerrain;
    return Availability::Blocked;
  }
  // Tech is checked BEFORE affordability so "locked" wins over "cannot afford". Telling a
  // player they are 3 stars short of a unit they have not unlocked is actively misleading.
  if (stats.needs_tech && !s.players[c.owner].has_tech(stats.tech)) {
    return Availability::MissingTech;
  }
  if (c.unit_count >= c.unit_capacity() || s.unit_count >= kMaxUnits) {
    reason = BlockReason::CityFull;
    return Availability::Blocked;
  }
  if (s.tiles[c.tile].unit != kNoUnit) {
    reason = BlockReason::TileOccupied;
    return Availability::Blocked;
  }
  if (s.players[c.owner].stars < stats.cost) return Availability::CannotAfford;
  return Availability::Available;
}

Availability can_research(const GameState& s, const Rules& rules, PlayerId player, Tech tech,
                          BlockReason& reason) {
  reason = BlockReason::None;
  const Player& p = s.players[player];
  if (p.has_tech(tech)) {
    reason = BlockReason::AlreadyBuilt;
    return Availability::Blocked;
  }
  // Backwards research: the edge is traversable in BOTH directions, so a tech is reachable if
  // its parent is owned OR any child of it is owned (resolutions.md R30).
  bool reachable = tech_is_root(tech) || p.has_tech(tech_parent(tech));
  if (!reachable) {
    for (int i = 0; i < kTechCount && !reachable; ++i) {
      const auto other = static_cast<Tech>(i);
      if (!tech_is_root(other) && tech_parent(other) == tech && p.has_tech(other)) reachable = true;
    }
  }
  if (!reachable) return Availability::MissingTech;

  int city_count = 0;
  for (CityId c = 0; c < s.city_count; ++c) {
    if (s.cities[c].owner == player) ++city_count;
  }
  if (p.stars < rules.tech_cost(tech, city_count, p.has_tech(Tech::Philosophy))) {
    return Availability::CannotAfford;
  }
  return Availability::Available;
}

// Roads are a tile FLAG rather than an Improvement, because a tile may carry a road AND a
// building. Buildable on your own or NEUTRAL territory, never enemy (resolutions.md R31).
Availability can_build_road(const GameState& s, const Rules&, TileIdx tile_idx,
                            BlockReason& reason) {
  reason = BlockReason::None;
  const Tile& tile = s.tiles[tile_idx];
  if (tile.has(TileFlag::Road) || tile.has(TileFlag::Bridge)) {
    reason = BlockReason::AlreadyBuilt;
    return Availability::Blocked;
  }
  if (tile.terrain == Terrain::Mountain || tile.terrain == Terrain::Ocean) {
    reason = BlockReason::WrongTerrain;  // no roads on mountains (live-rules.md §6.1)
    return Availability::Blocked;
  }
  if (tile.owner != kNoPlayer && tile.owner != s.current_player) {
    reason = BlockReason::NotYourTerritory;
    return Availability::Blocked;
  }
  const bool bridge = tile.terrain == Terrain::Water;
  if (!s.me().has_tech(bridge ? Tech::Sailing : Tech::Roads)) return Availability::MissingTech;
  const int cost = bridge ? 5 : 2;
  if (s.me().stars < cost) return Availability::CannotAfford;
  return Availability::Available;
}

// Embarking: a land unit standing on a friendly Port becomes a Raft carrying it. The hull has
// NO HP of its own -- it is the SAME entity and damage carries both ways (R29).
Availability can_embark(const GameState& s, const Rules& rules, TileIdx tile_idx,
                        BlockReason& reason) {
  reason = BlockReason::None;
  const UnitId id = s.tiles[tile_idx].unit;
  if (id == kNoUnit) {
    reason = BlockReason::NoResource;
    return Availability::Blocked;
  }
  const Unit& u = s.units[id];
  if (u.has(UnitFlag::Embarked) || s.tiles[tile_idx].improvement != Improvement::Port) {
    reason = BlockReason::WrongTerrain;
    return Availability::Blocked;
  }
  if (s.tiles[tile_idx].owner != u.owner) {
    reason = BlockReason::NotYourTerritory;
    return Availability::Blocked;
  }
  if (!s.players[u.owner].has_tech(Tech::Fishing)) return Availability::MissingTech;
  (void)rules;
  return Availability::Available;
}

// Raft -> Scout / Rammer / Bomber, in your own territory. Upgrading does NOT heal.
Availability can_upgrade_naval(const GameState& s, const Rules& rules, TileIdx tile_idx,
                               UnitType into, BlockReason& reason) {
  reason = BlockReason::None;
  const UnitId id = s.tiles[tile_idx].unit;
  if (id == kNoUnit || !s.units[id].has(UnitFlag::Embarked) ||
      s.units[id].type != UnitType::Raft) {
    reason = BlockReason::WrongTerrain;
    return Availability::Blocked;
  }
  if (into != UnitType::Scout && into != UnitType::Rammer && into != UnitType::Bomber) {
    reason = BlockReason::WrongTerrain;
    return Availability::Blocked;
  }
  if (s.tiles[tile_idx].owner != s.units[id].owner) {
    reason = BlockReason::NotYourTerritory;
    return Availability::Blocked;
  }
  const UnitStats& stats = rules.unit(into);
  if (stats.needs_tech && !s.me().has_tech(stats.tech)) return Availability::MissingTech;
  if (s.me().stars < stats.cost) return Availability::CannotAfford;
  return Availability::Available;
}

Availability can_build(const GameState& s, const Rules& rules, TileIdx tile_idx,
                       Improvement imp, BlockReason& reason) {
  reason = BlockReason::None;
  if (imp == Improvement::Road || imp == Improvement::Bridge) {
    const bool want_bridge = imp == Improvement::Bridge;
    const bool is_water = s.tiles[tile_idx].terrain == Terrain::Water;
    if (want_bridge != is_water) {
      reason = BlockReason::WrongTerrain;
      return Availability::Blocked;
    }
    return can_build_road(s, rules, tile_idx, reason);
  }
  if (imp == Improvement::None) {
    reason = BlockReason::WrongTerrain;
    return Availability::Blocked;
  }
  const Tile& tile = s.tiles[tile_idx];
  const ImprovementStats& stats = rules.improvement(imp);

  if (tile.owner != s.current_player) {
    reason = BlockReason::NotYourTerritory;
    return Availability::Blocked;
  }
  if ((stats.terrain_mask & terrain_bit(tile.terrain)) == 0) {
    reason = BlockReason::WrongTerrain;
    return Availability::Blocked;
  }
  if (stats.needs_tech && !s.me().has_tech(stats.tech)) return Availability::MissingTech;
  if (tile.improvement != Improvement::None) {
    reason = BlockReason::AlreadyBuilt;
    return Availability::Blocked;
  }
  if (stats.needs_resource && tile.resource != stats.resource) {
    reason = BlockReason::NoResource;
    return Availability::Blocked;
  }
  if (stats.per_city_limit > 0 && tile.city != kNoCity) {
    int existing = 0;
    for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
      if (s.tiles[t].city == tile.city && s.tiles[t].improvement == imp) ++existing;
    }
    if (existing >= stats.per_city_limit) {
      reason = BlockReason::PerCityLimit;
      return Availability::Blocked;
    }
  }
  if (s.me().stars < stats.cost) return Availability::CannotAfford;
  return Availability::Available;
}

Availability can_harvest(const GameState& s, const Rules& rules, TileIdx tile_idx,
                         BlockReason& reason) {
  reason = BlockReason::None;
  const Tile& tile = s.tiles[tile_idx];
  const ResourceStats& stats = rules.resource(tile.resource);
  if (tile.resource == Resource::None || !stats.harvestable) {
    reason = BlockReason::NoResource;
    return Availability::Blocked;
  }
  if (tile.owner != s.current_player) {
    reason = BlockReason::NotYourTerritory;
    return Availability::Blocked;
  }
  if (stats.needs_tech && !s.me().has_tech(stats.tech)) return Availability::MissingTech;
  if (s.me().stars < stats.harvest_cost) return Availability::CannotAfford;
  return Availability::Available;
}

void legal_select(const GameState& s, const Rules& rules, ActionMask& mask) {
  BlockReason scratch = BlockReason::None;
  // A unit is selectable if the enumerator offers it ANYTHING -- asked by calling the
  // enumerator, not by re-deriving the answer.
  //
  // This started as `unit_can_act(s, u)`, which means "can take a normal move/attack action",
  // and every verb that outlives a spent turn had to be bolted on beside it. Recover was
  // missing, so healing after moving was impossible. Then Promote was missing, so a unit that
  // moved and earned a veteran promotion could not cash it. The audit found each in turn,
  // which was the signal that patching verbs one at a time was the wrong shape of fix.
  //
  // The cost is acceptable precisely BECAUSE it is per unit: there are a few dozen units, not
  // a few hundred tiles, so this is nothing like enumerating every tile. The tile scan below
  // stays hand-rolled for exactly that reason.
  //
  // `unit_can_act` is checked FIRST as a fast accept. It is the common case -- most units have
  // not acted yet -- and legality generation is the search bottleneck this project already
  // paid to fix once (HANDOFF section 8: it was worth 3.75x). Enumerating every unit
  // unconditionally cost MCTS about 2x; enumerating only the units that have already acted
  // costs nothing measurable and still asks the enumerator the question that matters, which is
  // "does this spent unit have anything left".
  {
    std::vector<Option> unit_options;
    for (UnitId u = 0; u < s.unit_count; ++u) {
      const Unit& unit = s.units[u];
      if (!unit.alive() || unit.owner != s.current_player || unit.tile == kNoTile) continue;
      if (unit_can_act(s, u)) {
        mask.set(tile_to_slot(unit.tile));
        continue;
      }
      enumerate_options(s, rules, unit.tile, Layer::Unit, false, unit_options);
      if (!unit_options.empty()) mask.set(tile_to_slot(unit.tile));
    }
  }
  for (CityId c = 0; c < s.city_count; ++c) {
    const City& city = s.cities[c];
    if (city.owner != s.current_player) continue;
    // A pending level-up must be resolved; while one is outstanding the city is always
    // selectable so play cannot continue around it.
    if (city.pending_rewards > 0) {
      mask.set(tile_to_slot(city.tile));
      continue;
    }
    for (int i = 0; i < kUnitTypeCount; ++i) {
      if (can_train(s, rules, c, static_cast<UnitType>(i), scratch) == Availability::Available) {
        mask.set(tile_to_slot(city.tile));
        break;
      }
    }
  }
  for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
    if (can_harvest(s, rules, t, scratch) == Availability::Available) {
      mask.set(tile_to_slot(t));
      continue;
    }
    // Hoist the territory test OUT of the improvement loop. Every tile used to be tried
    // against all 15 improvements even in enemy territory, where nothing is ever buildable.
    // This is the hot path: legal_actions is called once per search node, so a constant
    // factor here is a constant factor on every agent in the ladder.
    const PlayerId owner = s.tiles[t].owner;
    if (owner != s.current_player && owner != kNoPlayer) continue;  // enemy: nothing at all
    // Scorched earth: an owned tile carrying a building can always be Destroyed, and that is
    // frequently the ONLY thing left to do with it -- a finished Farm has no resource to
    // harvest and no room to build. The enumerator offered Destroy on every such tile while
    // this function never handed the tile out, so the option was unreachable: in the browser
    // the button appeared and `act` answered "action refused".
    if (owner == s.current_player && s.tiles[t].improvement != Improvement::None) {
      mask.set(tile_to_slot(t));
      continue;
    }
    const int first = (owner == s.current_player) ? 1 : static_cast<int>(Improvement::Road);
    const int last = static_cast<int>(Improvement::Count);
    for (int i = first; i < last; ++i) {
      if (can_build(s, rules, t, static_cast<Improvement>(i), scratch) == Availability::Available) {
        mask.set(tile_to_slot(t));
        break;
      }
    }
  }
  // The global pseudo-actor carries research and end-turn.
  mask.set(kGlobalActorSlot);
}

void legal_verbs(const GameState& s, const Rules& rules, ActionMask& mask) {
  BlockReason scratch = BlockReason::None;
  if (s.global_actor) {
    for (int i = 0; i < kTechCount; ++i) {
      if (can_research(s, rules, s.current_player, static_cast<Tech>(i), scratch) == Availability::Available) {
        mask.set(static_cast<int>(Verb::Research));
        break;
      }
    }
    mask.set(static_cast<int>(Verb::EndTurn));
    mask.set(static_cast<int>(Verb::Deselect));
    return;
  }

  const TileIdx tile_idx = s.selected;
  const Tile& tile = s.tiles[tile_idx];
  const CityId city_id = city_at(s, tile_idx);
  const UnitId unit_id = tile.unit;

  if (city_id != kNoCity && s.cities[city_id].owner == s.current_player) {
    if (s.cities[city_id].pending_rewards > 0) {
      // Nothing else is legal until the level-up is resolved.
      mask.set(static_cast<int>(Verb::ChooseReward));
      mask.set(static_cast<int>(Verb::Deselect));
      return;
    }
    for (int i = 0; i < kUnitTypeCount; ++i) {
      if (can_train(s, rules, city_id, static_cast<UnitType>(i), scratch) == Availability::Available) {
        mask.set(static_cast<int>(Verb::Train));
        break;
      }
    }
  }

  if (unit_can_act(s, unit_id)) {
    const Unit& u = s.units[unit_id];
    const UnitStats& stats = rules.unit(u.type);
    const bool has_dash = has_skill(static_cast<Skill>(stats.skills), Skill::Dash);
    const bool has_escape = has_skill(static_cast<Skill>(stats.skills), Skill::Escape);

    // Dash and Escape are ACTION-SPACE modifiers, not stat modifiers (resolutions.md R26):
    // they decide which verbs survive after another verb resolves.
    const bool may_move = u.moves_left > 0 && (!u.has(UnitFlag::HasAttacked) || has_escape);
    const bool may_attack = !u.has(UnitFlag::HasAttacked) &&
                            (!u.has(UnitFlag::HasMoved) || has_dash) && stats.attack > 0.0f;

    if (may_move) {
      ActionMask moves;
      reachable_tiles(s, rules, unit_id, moves);
      if (!moves.empty()) mask.set(static_cast<int>(Verb::Move));
    }
    if (may_attack) {
      const Coord c = s.dims.coord(u.tile);
      for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
        const UnitId target = s.tiles[t].unit;
        if (target == kNoUnit || s.units[target].owner == u.owner) continue;
        if (!s.units[target].alive()) continue;
        const Coord tc = s.dims.coord(t);
        const int distance = std::max(std::abs(c.x - tc.x), std::abs(c.y - tc.y));
        if (distance <= stats.range) {
          mask.set(static_cast<int>(Verb::Attack));
          break;
        }
      }
    }
    // Capture and Examine require having STARTED the turn on the tile (resolutions.md R25).
    const bool started_here = !u.has(UnitFlag::ArrivedThisTurn);
    if (started_here && rules.capture_requires_turn_start) {
      const bool capturable_village = tile.resource == Resource::Village;
      const bool capturable_city = city_id != kNoCity && s.cities[city_id].owner != u.owner;
      if (capturable_village || capturable_city) mask.set(static_cast<int>(Verb::Capture));
      if (tile.resource == Resource::Ruin) mask.set(static_cast<int>(Verb::Examine));
    }
  }

  if (can_harvest(s, rules, tile_idx, scratch) == Availability::Available) {
    mask.set(static_cast<int>(Verb::Harvest));
  }

  // The agent's verb set is derived from the SAME option enumerator the UI uses, so the two
  // cannot offer different things. This is the projection described in options.hpp.
  {
    std::vector<Option> options;
    Layer layers[4];
    const int count = tile_layers(s, tile_idx, layers);
    for (int i = 0; i < count; ++i) {
      enumerate_options(s, rules, tile_idx, layers[i], false, options);
      for (const Option& o : options) mask.set(static_cast<int>(o.verb));
    }
  }
  // There used to be a second, independent `can_build` loop here, directly beneath the comment
  // promising the mask is a projection of the enumerator. It drifted exactly where you would
  // expect: `tile_layers` deliberately gives a CITY tile no Tile layer ("you cannot build under
  // your own city" -- owner-confirmed), so the enumerator offered no Build there while this
  // loop put Build in the mask anyway. The agent could build on a city tile and the human had
  // no menu to do it from. The union above already covers Build wherever a Tile layer exists,
  // so the loop was redundant as well as wrong.

  mask.set(static_cast<int>(Verb::Deselect));
}

void legal_targets(const GameState& s, const Rules& rules, ActionMask& mask) {
  BlockReason scratch = BlockReason::None;
  const TileIdx tile_idx = s.selected;
  switch (s.pending_verb) {
    case Verb::Move: {
      reachable_tiles(s, rules, s.tiles[tile_idx].unit, mask);
      break;
    }
    case Verb::Attack: {
      const UnitId attacker = s.tiles[tile_idx].unit;
      const Unit& u = s.units[attacker];
      const Coord c = s.dims.coord(u.tile);
      const int range = rules.unit(u.type).range;
      for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
        const UnitId target = s.tiles[t].unit;
        if (target == kNoUnit || s.units[target].owner == u.owner) continue;
        if (!s.units[target].alive()) continue;
        const Coord tc = s.dims.coord(t);
        if (std::max(std::abs(c.x - tc.x), std::abs(c.y - tc.y)) <= range) {
          mask.set(tile_to_slot(t));
        }
      }
      break;
    }
    case Verb::Train: {
      const CityId city_id = city_at(s, tile_idx);
      for (int i = 0; i < kUnitTypeCount; ++i) {
        if (can_train(s, rules, city_id, static_cast<UnitType>(i), scratch) == Availability::Available) {
          mask.set(unit_type_to_slot(static_cast<UnitType>(i)));
        }
      }
      break;
    }
    case Verb::Research: {
      for (int i = 0; i < kTechCount; ++i) {
        if (can_research(s, rules, s.current_player, static_cast<Tech>(i), scratch) == Availability::Available) {
          mask.set(tech_to_slot(static_cast<Tech>(i)));
        }
      }
      break;
    }
    case Verb::Build: {
      for (int i = 1; i < static_cast<int>(Improvement::Count); ++i) {
        if (can_build(s, rules, tile_idx, static_cast<Improvement>(i), scratch) == Availability::Available) {
          mask.set(improvement_to_slot(static_cast<Improvement>(i)));
        }
      }
      break;
    }
    case Verb::ChooseReward: {
      const CityId city_id = city_at(s, tile_idx);
      const int level = reward_menu_level(s.cities[city_id]);
      // live-rules.md §4.3. Levels 5+ repeat the Park / SuperUnit choice.
      if (level == 2) {
        mask.set(reward_to_slot(CityReward::Workshop));
        mask.set(reward_to_slot(CityReward::Explorer));
      } else if (level == 3) {
        mask.set(reward_to_slot(CityReward::CityWall));
        mask.set(reward_to_slot(CityReward::Resources));
      } else if (level == 4) {
        mask.set(reward_to_slot(CityReward::PopulationGrowth));
        mask.set(reward_to_slot(CityReward::BorderGrowth));
      } else {
        mask.set(reward_to_slot(CityReward::Park));
        mask.set(reward_to_slot(CityReward::SuperUnit));
      }
      break;
    }
    case Verb::Convert: {
      const UnitId id = s.tiles[tile_idx].unit;
      const Coord c = s.dims.coord(tile_idx);
      for (const Coord off : kNeighbourOffsets) {
        const Coord n{static_cast<std::int16_t>(c.x + off.x),
                      static_cast<std::int16_t>(c.y + off.y)};
        if (!s.dims.contains(n)) continue;
        const TileIdx t = s.dims.index(n);
        const UnitId other = s.tiles[t].unit;
        if (other != kNoUnit && s.units[other].alive() &&
            s.units[other].owner != s.units[id].owner) {
          mask.set(tile_to_slot(t));
        }
      }
      break;
    }
    case Verb::UpgradeNaval: {
      BlockReason why = BlockReason::None;
      for (const UnitType into : {UnitType::Scout, UnitType::Rammer, UnitType::Bomber}) {
        if (can_upgrade_naval(s, rules, tile_idx, into, why) == Availability::Available) {
          mask.set(unit_type_to_slot(into));
        }
      }
      break;
    }
    case Verb::Recover:
    case Verb::Promote:
    case Verb::Heal:
    case Verb::Disband:
    case Verb::Destroy:
    case Verb::Harvest:
    case Verb::Capture:
    case Verb::Examine:
      // Self-targeting verbs: the actor's own tile is the only target.
      mask.set(tile_to_slot(tile_idx));
      break;
    default: break;
  }
}

}  // namespace

void legal_actions(const GameState& s, const Rules& rules, ActionMask& mask) {
  mask.clear();
  if (outcome(s, rules).has_value()) return;

  switch (s.phase) {
    case Phase::Select: legal_select(s, rules, mask); break;
    case Phase::Verb: legal_verbs(s, rules, mask); break;
    case Phase::Target: legal_targets(s, rules, mask); break;
    default: break;
  }

  // A player must never be stuck with nothing to do.
  if (mask.empty()) {
    if (s.phase == Phase::Select) {
      mask.set(kGlobalActorSlot);
    } else {
      mask.set(static_cast<int>(Verb::Deselect));
    }
  }
}

// -----------------------------------------------------------------------------------------
// Apply
// -----------------------------------------------------------------------------------------

namespace {

void kill_unit(GameState& s, UnitId id) {
  Unit& u = s.units[id];
  if (u.tile != kNoTile && s.tiles[u.tile].unit == id) s.tiles[u.tile].unit = kNoUnit;
  if (u.home_city != kNoCity && s.cities[u.home_city].unit_count > 0) {
    s.cities[u.home_city].unit_count =
        static_cast<std::uint8_t>(s.cities[u.home_city].unit_count - 1);
  }
  u.hp = 0;
  u.tile = kNoTile;
}

// Finds a free tile for a spawned unit, preferring `preferred` then its neighbours.
TileIdx free_spawn_tile(const GameState& s, const Rules& rules, PlayerId owner, TileIdx preferred) {
  if (preferred >= 0 && s.tiles[preferred].unit == kNoUnit &&
      tribe_can_enter(s, rules, owner, s.tiles[preferred])) {
    return preferred;
  }
  const Coord c = s.dims.coord(preferred);
  for (const Coord off : kNeighbourOffsets) {
    const Coord n{static_cast<std::int16_t>(c.x + off.x), static_cast<std::int16_t>(c.y + off.y)};
    if (!s.dims.contains(n)) continue;
    const TileIdx t = s.dims.index(n);
    if (s.tiles[t].unit == kNoUnit && tribe_can_enter(s, rules, owner, s.tiles[t])) return t;
  }
  return kNoTile;
}

UnitId spawn_unit(GameState& s, const Rules& rules, UnitType type, PlayerId owner, TileIdx tile,
                  CityId home, bool veteran) {
  if (s.unit_count >= kMaxUnits || tile == kNoTile) return kNoUnit;
  const UnitId id = s.unit_count++;
  Unit& u = s.units[id];
  u = Unit{};
  u.type = type;
  u.carried_type = type;
  u.owner = owner;
  u.tile = tile;
  u.home_city = home;
  if (veteran) u.add(UnitFlag::Veteran);
  u.hp = static_cast<std::int16_t>(max_hp_for(rules, u));
  u.add(UnitFlag::FreshlyTrained);
  s.tiles[tile].unit = id;
  if (home != kNoCity) {
    s.cities[home].unit_count = static_cast<std::uint8_t>(s.cities[home].unit_count + 1);
  }
  return id;
}

// The Explorer is not a unit you command -- it is an EVENT. It walks its whole budget the
// moment it is granted, revealing fog, then vanishes. Modelling it as a controllable unit is
// why it appeared not to work at all: it spawned with no moves and nothing ever drove it.
void run_explorer(GameState& s, const Rules& rules, PlayerId owner, TileIdx from,
                  std::vector<std::string>* log) {
  const int budget = rules.unit(UnitType::Explorer).move;  // 12 in v2.16.3, not the wiki's 15
  TileIdx at = from;
  int revealed = 0;
  for (int step = 0; step < budget; ++step) {
    // Walk toward unexplored ground, preferring tiles that reveal the most. Ties broken from
    // the STATE's rng so a replay reproduces the same walk.
    const Coord c = s.dims.coord(at);
    TileIdx best = kNoTile;
    int best_gain = -1;
    for (int i = 0; i < kNeighbourCount; ++i) {
      const Coord off = kNeighbourOffsets[(i + static_cast<int>(s.rng.bounded(kNeighbourCount))) %
                                          kNeighbourCount];
      const Coord n{static_cast<std::int16_t>(c.x + off.x),
                    static_cast<std::int16_t>(c.y + off.y)};
      if (!s.dims.contains(n)) continue;
      const TileIdx t = s.dims.index(n);
      // The Explorer ignores units and terrain cost but still respects tech gating on
      // mountains and water, per live-rules.md §4.3.
      if (s.tiles[t].terrain == Terrain::Mountain && !s.players[owner].has_tech(Tech::Climbing)) {
        continue;
      }
      if (is_water(s.tiles[t].terrain) && !s.players[owner].has_tech(Tech::Sailing)) continue;

      int gain = 0;
      const Coord tc = s.dims.coord(t);
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const Coord look{static_cast<std::int16_t>(tc.x + dx),
                           static_cast<std::int16_t>(tc.y + dy)};
          if (s.dims.contains(look) && !s.explored[owner].test(s.dims.index(look))) ++gain;
        }
      }
      if (gain > best_gain) {
        best_gain = gain;
        best = t;
      }
    }
    if (best == kNoTile) break;
    at = best;
    const Coord ac = s.dims.coord(at);
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        const Coord look{static_cast<std::int16_t>(ac.x + dx),
                         static_cast<std::int16_t>(ac.y + dy)};
        if (!s.dims.contains(look)) continue;
        const TileIdx t = s.dims.index(look);
        if (!s.explored[owner].test(t)) ++revealed;
        s.explored[owner].set(t);
      }
    }
  }
  check_meetings(s, rules, owner, log);
  if (log) {
    log->push_back("Explorer revealed " + std::to_string(revealed) + " tiles");
  }
}

void capture_city(GameState& s, const Rules& rules, CityId city_id, PlayerId new_owner,
                  std::vector<std::string>* log) {
  City& c = s.cities[city_id];
  const PlayerId old_owner = c.owner;
  c.owner = new_owner;

  // Territory transfers with the city, keeping its Border Growth (resolutions.md R16).
  for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
    if (s.tiles[t].city == city_id) s.tiles[t].owner = new_owner;
  }
  // Ownership is an INPUT to every adjacency yield -- `count_adjacent_improvements` only counts
  // neighbours owned by the same player -- so re-parenting a city's territory silently changes
  // what its border buildings are worth, and what the neighbouring player's are worth too.
  // Nothing recomputed that, so a captured border left stale population banked on both sides.
  resync_population_board(s, rules, log);
  // City level is retained. Population is NOT recomputed into a level change -- if the trade
  // network loss drives it negative it stays negative and the level holds (R17).
  if (c.has(CityFlag::Capital) && old_owner != kNoPlayer) {
    c.population = static_cast<std::int16_t>(c.population - rules.connection_population);
  }
  if (log) {
    log->push_back("Player " + std::to_string(new_owner) + " captured a city");
  }

  // Domination is decided on capitals, so a player who has lost every city is out.
  if (old_owner != kNoPlayer) {
    bool has_city = false;
    for (CityId other = 0; other < s.city_count; ++other) {
      if (s.cities[other].owner == old_owner) has_city = true;
    }
    if (!has_city) s.players[old_owner].alive = false;
  }
}

void grant_reward(GameState& s, const Rules& rules, CityId city_id, CityReward reward,
                  std::vector<std::string>* log) {
  City& c = s.cities[city_id];
  switch (reward) {
    case CityReward::Workshop: c.add(CityFlag::Workshop); break;
    case CityReward::CityWall: c.add(CityFlag::Walled); break;
    case CityReward::Resources: s.players[c.owner].stars += 5; break;
    case CityReward::PopulationGrowth: add_population(s, rules, city_id, 3, log); break;
    case CityReward::BorderGrowth:
      c.add(CityFlag::BorderGrown);
      claim_territory_and_resync(s, rules, city_id, rules.grown_city_border, log);
      break;
    case CityReward::Park: c.park_count = static_cast<std::uint8_t>(c.park_count + 1); break;
    case CityReward::Explorer:
      run_explorer(s, rules, c.owner, c.tile, log);
      break;
    case CityReward::SuperUnit: {
      // Spawning a super unit DISPLACES whatever stands on the city, or kills it if the city
      // is completely surrounded (resolutions.md R27). This is the counter to sitting on a
      // city to besiege it, and is documented nowhere -- it came from the owner.
      const TileIdx tile = c.tile;
      const UnitId occupant = s.tiles[tile].unit;
      if (occupant != kNoUnit) {
        const TileIdx shove = free_spawn_tile(s, rules, s.units[occupant].owner, tile);
        s.tiles[tile].unit = kNoUnit;
        if (shove != kNoTile && shove != tile) {
          s.units[occupant].tile = shove;
          s.tiles[shove].unit = occupant;
          if (log) log->push_back("The Giant shoved a unit aside");
        } else {
          kill_unit(s, occupant);
          if (log) log->push_back("The Giant crushed a surrounded unit");
        }
      }
      spawn_unit(s, rules, UnitType::Giant, c.owner, tile, city_id, false);
      if (log) log->push_back("A Giant arrives");
      break;
    }
    default:
      break;
  }
  c.pending_rewards = static_cast<std::int8_t>(c.pending_rewards - 1);
}

}  // namespace

bool apply(GameState& s, const Rules& rules, Action action, std::vector<std::string>* log,
           const ActionMask* precomputed) {
  // Legality is still the engine's job -- this only avoids RECOMPUTING it. A search has
  // already built the mask for exactly this state in order to pick the action, and paying
  // for it a second time inside apply was roughly half the cost of a search node.
  ActionMask local;
  const ActionMask* mask = precomputed;
  if (mask == nullptr) {
    legal_actions(s, rules, local);
    mask = &local;
  }
  if (action.index < 0 || action.index >= kActionSlots || !mask->test(action.index)) return false;

  const int index = action.index;

  if (s.phase == Phase::Select) {
    if (index == kGlobalActorSlot) {
      s.global_actor = true;
      s.selected = kNoTile;
    } else {
      s.global_actor = false;
      s.selected = slot_to_tile(index);
    }
    s.phase = Phase::Verb;
    return true;
  }

  if (s.phase == Phase::Verb) {
    const auto verb = static_cast<Verb>(index);
    if (verb == Verb::Deselect) {
      s.phase = Phase::Select;
      s.selected = kNoTile;
      s.global_actor = false;
      return true;
    }
    if (verb == Verb::EndTurn) {
      advance_player(s, rules);
      return true;
    }
    s.pending_verb = verb;
    s.phase = Phase::Target;
    return true;
  }

  // Target phase.
  const TileIdx actor_tile = s.selected;
  switch (s.pending_verb) {
    case Verb::Move: {
      const TileIdx destination = slot_to_tile(index);
      const UnitId id = s.tiles[actor_tile].unit;
      Unit& u = s.units[id];
      s.tiles[actor_tile].unit = kNoUnit;
      u.tile = destination;
      s.tiles[destination].unit = id;

      // Embark / disembark. A naval unit is the SAME entity as the land unit it carries:
      // carried_type remembers what to restore, and HP passes through untouched in both
      // directions (resolutions.md R29).
      const bool onto_water = is_water(s.tiles[destination].terrain);
      if (onto_water && !u.has(UnitFlag::Embarked)) {
        u.carried_type = u.type;
        u.type = UnitType::Raft;
        u.add(UnitFlag::Embarked);
      } else if (!onto_water && u.has(UnitFlag::Embarked)) {
        u.type = u.carried_type;
        u.remove(UnitFlag::Embarked);
      }

      u.add(UnitFlag::HasMoved);
      u.add(UnitFlag::ArrivedThisTurn);
      u.moves_used = 1;
      u.moves_left = 0;
      // Moving ends the turn unless the unit has Dash (resolutions.md R26).
      if (!has_skill(static_cast<Skill>(rules.unit(u.type).skills), Skill::Dash) ||
          u.has(UnitFlag::HasAttacked)) {
        u.add(UnitFlag::TurnEnded);
      }
      recompute_visibility(s, rules, u.owner);
      check_meetings(s, rules, u.owner, log);
      update_siege_flags(s);
      break;
    }
    case Verb::Attack: {
      const UnitId attacker = s.tiles[actor_tile].unit;
      const UnitId defender = s.tiles[slot_to_tile(index)].unit;
      const CombatResult combat = resolve_combat(s, rules, attacker, defender);
      s.units[defender].hp = static_cast<std::int16_t>(s.units[defender].hp - combat.attacker_damage);
      if (combat.retaliates) {
        s.units[attacker].hp =
            static_cast<std::int16_t>(s.units[attacker].hp - combat.defender_damage);
      }
      if (log) {
        log->push_back("Attack for " + std::to_string(combat.attacker_damage / kHpScale) +
                       (combat.retaliates
                            ? ", retaliation " + std::to_string(combat.defender_damage / kHpScale)
                            : ", no retaliation"));
      }
      const TileIdx target_tile = s.units[defender].tile;
      const bool killed = !s.units[defender].alive();
      // Kills count toward promotion by ATTACK OR RETALIATION (live-rules.md §1.13), so the
      // defender is credited too when it kills the attacker on the counter.
      if (killed) s.units[attacker].kills = static_cast<std::uint8_t>(s.units[attacker].kills + 1);
      if (combat.retaliates && !s.units[attacker].alive()) {
        s.units[defender].kills = static_cast<std::uint8_t>(s.units[defender].kills + 1);
      }
      if (killed) kill_unit(s, defender);
      Unit& a = s.units[attacker];

      // Advance after kill: a MELEE unit takes the place of what it killed (live-rules.md
      // §1.7). The wiki never defines melee numerically, but every melee unit has range 1 and
      // the Archer (range 2) is the stated counterexample, so range == 1 is the definition.
      if (killed && a.alive() && rules.unit(a.type).range == 1 &&
          s.tiles[target_tile].unit == kNoUnit &&
          tribe_can_enter(s, rules, a.owner, s.tiles[target_tile])) {
        s.tiles[a.tile].unit = kNoUnit;
        a.tile = target_tile;
        s.tiles[target_tile].unit = attacker;
        a.add(UnitFlag::ArrivedThisTurn);
        recompute_visibility(s, rules, a.owner);
        check_meetings(s, rules, a.owner, log);
      }
      if (!a.alive()) {
        kill_unit(s, attacker);
      } else {
        const auto skills = static_cast<Skill>(rules.unit(a.type).skills);
        // PERSIST: a Knight that KILLS keeps its attack (live-rules.md §2.3). This is what
        // lets one Knight run a line of wounded units, and it was declared in content.hpp,
        // assigned to the Knight in rules.cpp and asserted in test_content -- while game.cpp
        // never mentioned the skill at all, so every Knight's turn ended on its first kill.
        // Bounded by construction: it only re-arms on a kill, and kills are finite.
        //
        // Written as a condition rather than an early `break`: breaking here would skip the
        // update_siege_flags() below and leave a city's Besieged flag stale after a Knight
        // cleared the unit standing on it -- a silent income bug.
        const bool persists = killed && has_skill(skills, Skill::Persist);
        a.moves_used = 1;
        if (persists) {
          // Keep the attack AND the ability to keep riding: Persist is a chain, and a Knight
          // that killed something adjacent has usually just advanced onto its tile.
          a.moves_left = static_cast<std::uint8_t>(rules.unit(a.type).move);
        } else if (!has_skill(skills, Skill::Escape)) {
          a.add(UnitFlag::HasAttacked);
          // Attacking ends the turn unless the unit has Escape (R26).
          a.add(UnitFlag::TurnEnded);
        } else {
          a.add(UnitFlag::HasAttacked);
          // ESCAPE grants a FRESH retreat move. `moves_left` was already spent if the unit
          // rode in to make this attack (Verb::Move zeroes it), and nothing restored it -- so
          // move -> attack -> move, the Rider's entire reason for having both Dash and Escape,
          // was dead while attack -> move worked. That asymmetry is why the first test of
          // Escape passed: it attacked from a standing start.
          //
          // Self-limiting: the retreat Move re-zeroes the budget and, because HasAttacked is
          // now set, also sets TurnEnded -- so there is no second escape and no attack after.
          a.moves_left = static_cast<std::uint8_t>(rules.unit(a.type).move);
        }
      }
      update_siege_flags(s);
      break;
    }
    case Verb::Capture: {
      const UnitId id = s.tiles[actor_tile].unit;
      Tile& tile = s.tiles[actor_tile];
      if (tile.resource == Resource::Village) {
        tile.resource = Resource::None;
        const CityId city_id = s.city_count++;
        City& c = s.cities[city_id];
        c = City{};
        c.owner = s.current_player;
        c.tile = actor_tile;
        c.level = 1;
        claim_territory_and_resync(s, rules, city_id, rules.new_city_border, log);
        if (log) log->push_back("Captured a village");
      } else {
        const CityId city_id = city_at(s, actor_tile);
        if (city_id != kNoCity) capture_city(s, rules, city_id, s.current_player, log);
      }
      // Capturing ALWAYS ends the unit's turn (R26).
      s.units[id].add(UnitFlag::TurnEnded);
      s.units[id].moves_left = 0;
      recompute_visibility(s, rules, s.current_player);
      update_siege_flags(s);
      break;
    }
    case Verb::Harvest: {
      Tile& tile = s.tiles[actor_tile];
      const ResourceStats& stats = rules.resource(tile.resource);
      s.me().stars -= stats.harvest_cost;
      s.me().stars += stats.stars;
      if (tile.city != kNoCity) add_population(s, rules, tile.city, stats.population, log);
      tile.resource = Resource::None;
      break;
    }
    case Verb::Build: {
      Tile& tile = s.tiles[actor_tile];
      const auto imp = slot_to_improvement(index);
      if (imp == Improvement::Road || imp == Improvement::Bridge) {
        const bool bridge = tile.terrain == Terrain::Water;
        s.me().stars -= bridge ? 5 : 2;
        tile.add(bridge ? TileFlag::Bridge : TileFlag::Road);
        recompute_city_connections(s, rules, s.current_player);
        if (log) log->push_back(bridge ? "Built a bridge" : "Built a road");
        break;
      }
      const ImprovementStats& stats = rules.improvement(imp);
      s.me().stars -= stats.cost;
      tile.improvement = imp;
      // The yield is NOT computed here. Placing the building changes the neighbourhood, and
      // the neighbourhood is what decides every adjacency yield -- including the yields of
      // buildings that were already standing. Recomputing the patch is what makes a Windmill
      // built before its Farms come good when they arrive.
      resync_population_around(s, rules, actor_tile, log);
      break;
    }
    case Verb::Train: {
      const CityId city_id = city_at(s, actor_tile);
      const auto type = slot_to_unit_type(index);
      const UnitStats& stats = rules.unit(type);
      s.me().stars -= stats.cost;
      const UnitId id = s.unit_count++;
      Unit& u = s.units[id];
      u = Unit{};
      u.type = type;
      u.carried_type = type;
      u.owner = s.current_player;
      u.tile = actor_tile;
      u.hp = to_hp(stats.hp);
      u.home_city = city_id;
      u.moves_left = 0;
      // Cannot act until the owner's next turn (resolutions.md R2).
      u.add(UnitFlag::FreshlyTrained);
      s.tiles[actor_tile].unit = id;
      s.cities[city_id].unit_count =
          static_cast<std::uint8_t>(s.cities[city_id].unit_count + 1);
      if (log) log->push_back("Trained a unit");
      break;
    }
    case Verb::Research: {
      const Tech tech = slot_to_tech(index);
      int city_count = 0;
      for (CityId c = 0; c < s.city_count; ++c) {
        if (s.cities[c].owner == s.current_player) ++city_count;
      }
      s.me().stars -= rules.tech_cost(tech, city_count, s.me().has_tech(Tech::Philosophy));
      s.me().grant_tech(tech);
      if (log) log->push_back("Researched a technology");
      break;
    }
    case Verb::ChooseReward: {
      grant_reward(s, rules, city_at(s, actor_tile), slot_to_reward(index), log);
      break;
    }
    case Verb::Recover: {
      const UnitId id = s.tiles[actor_tile].unit;
      Unit& u = s.units[id];
      // +4 in your own territory, +2 in neutral OR enemy -- both give 2 (live-rules.md §1.12).
      const int amount = (s.tiles[actor_tile].owner == u.owner) ? 4 : 2;
      u.hp = static_cast<std::int16_t>(
          std::min(max_hp_for(rules, u), u.hp + amount * kHpScale));
      u.add(UnitFlag::HasRecovered);
      u.add(UnitFlag::TurnEnded);
      u.moves_left = 0;
      if (log) log->push_back("Recovered " + std::to_string(amount) + " HP");
      break;
    }
    case Verb::Promote: {
      const UnitId id = s.tiles[actor_tile].unit;
      Unit& u = s.units[id];
      // maxHealth += 5, THEN heal completely. No other stat changes. Once per unit.
      u.add(UnitFlag::Veteran);
      u.hp = static_cast<std::int16_t>(max_hp_for(rules, u));
      if (log) log->push_back("Promoted to veteran");
      break;
    }
    case Verb::Heal: {
      // Mind Bender: heals ALL adjacent friendlies by up to 4, and consumes its whole turn.
      const UnitId id = s.tiles[actor_tile].unit;
      Unit& healer = s.units[id];
      const Coord c = s.dims.coord(actor_tile);
      int healed = 0;
      for (const Coord off : kNeighbourOffsets) {
        const Coord n{static_cast<std::int16_t>(c.x + off.x),
                      static_cast<std::int16_t>(c.y + off.y)};
        if (!s.dims.contains(n)) continue;
        const UnitId other = s.tiles[s.dims.index(n)].unit;
        if (other == kNoUnit || !s.units[other].alive()) continue;
        if (s.units[other].owner != healer.owner) continue;
        const int before = s.units[other].hp;
        s.units[other].hp = static_cast<std::int16_t>(
            std::min(max_hp_for(rules, s.units[other]), s.units[other].hp + 4 * kHpScale));
        if (s.units[other].hp > before) ++healed;
      }
      healer.add(UnitFlag::TurnEnded);
      healer.moves_left = 0;
      if (log) log->push_back("Healed " + std::to_string(healed) + " nearby units");
      break;
    }
    case Verb::Convert: {
      // The Mind Bender has attack 0, so this deals no damage -- the target keeps its HP and
      // its veteran progress, and simply changes side.
      const UnitId id = s.tiles[actor_tile].unit;
      const UnitId target = s.tiles[slot_to_tile(index)].unit;
      Unit& converted = s.units[target];
      if (converted.home_city != kNoCity && s.cities[converted.home_city].unit_count > 0) {
        s.cities[converted.home_city].unit_count =
            static_cast<std::uint8_t>(s.cities[converted.home_city].unit_count - 1);
      }
      converted.owner = s.units[id].owner;
      converted.home_city = kNoCity;
      converted.add(UnitFlag::TurnEnded);
      s.units[id].add(UnitFlag::TurnEnded);
      s.units[id].moves_left = 0;
      recompute_visibility(s, rules, converted.owner);
      if (log) log->push_back("Converted an enemy unit");
      break;
    }
    case Verb::UpgradeNaval: {
      const UnitId id = s.tiles[actor_tile].unit;
      const auto into = slot_to_unit_type(index);
      s.me().stars -= rules.unit(into).cost;
      // Upgrading does NOT heal: the hull carries no HP of its own, so the embarked unit's
      // health passes straight through (resolutions.md R29).
      s.units[id].type = into;
      s.units[id].add(UnitFlag::TurnEnded);
      s.units[id].moves_left = 0;
      if (log) log->push_back("Upgraded a naval unit");
      break;
    }
    case Verb::Disband: {
      const UnitId id = s.tiles[actor_tile].unit;
      // Disband refunds half the unit's cost, rounded down.
      s.me().stars += rules.unit(s.units[id].type).cost / 2;
      kill_unit(s, id);
      if (log) log->push_back("Disbanded a unit");
      break;
    }
    case Verb::Destroy: {
      // Scorched earth: the documented counterplay to losing a city, which only makes sense
      // because a captured city otherwise inherits everything.
      s.tiles[actor_tile].improvement = Improvement::None;
      s.tiles[actor_tile].improvement_level = 0;
      // Scorched earth has to take the population BACK -- both this tile's own yield and any
      // adjacency bonus it was feeding to a neighbouring Windmill, Sawmill or Forge. Destroy
      // previously removed the building and left the population it had granted, so razing a
      // Farm made the city keep the food.
      resync_population_around(s, rules, actor_tile, log);
      if (log) log->push_back("Destroyed a building");
      break;
    }
    case Verb::Examine: {
      Tile& tile = s.tiles[actor_tile];
      tile.resource = Resource::None;

      // Uniform over the ELIGIBLE options (live-rules.md §8.2) -- the eligible set is
      // state-dependent, so the effective distribution shifts with game state. Drawn from the
      // STATE's rng so replays reproduce. UNRESOLVED #17: v2.16.3 says "changed how ruin
      // rewards are calculated" with no detail, so the uniform claim may be stale.
      // The rules DATA lists six ruin rewards (live.json resources.ruin.rewards: resources,
      // scrolls_of_wisdom, population, explorer, new_friends, rammer) and the audit documents
      // the water case as a veteran Rammer (polyenv-audit.md). Only four were implemented, so
      // a ruin on water could yield nothing but stars, tech or population -- the two naval and
      // exploratory outcomes simply did not exist.
      enum Reward { kResources, kScrolls, kPopulation, kNewFriends, kExplorer, kRewardCount };
      int eligible[kRewardCount];
      int count = 0;
      eligible[count++] = kResources;  // always available

      Tech free_tech = Tech::Climbing;
      bool has_free_tech = false;
      for (int i = 0; i < kTechCount; ++i) {
        const auto t = static_cast<Tech>(i);
        BlockReason why = BlockReason::None;
        if (!s.me().has_tech(t) && (tech_is_root(t) || s.me().has_tech(tech_parent(t)))) {
          free_tech = t;
          has_free_tech = true;
          (void)why;
          break;
        }
      }
      if (has_free_tech) eligible[count++] = kScrolls;

      CityId capital = kNoCity;
      for (CityId c = 0; c < s.city_count; ++c) {
        if (s.cities[c].owner == s.current_player && s.cities[c].has(CityFlag::Capital)) {
          capital = c;
        }
      }
      if (capital != kNoCity) eligible[count++] = kPopulation;

      const bool on_water = tile.terrain == Terrain::Water || tile.terrain == Terrain::Ocean;
      // New Friends is a veteran Swordsman on land and a veteran Rammer at sea -- the same
      // reward, hulled for the terrain it has to stand on.
      if (s.unit_count < kMaxUnits) eligible[count++] = kNewFriends;
      eligible[count++] = kExplorer;

      const int choice = eligible[s.rng.bounded(static_cast<std::uint32_t>(count))];
      switch (choice) {
        case kScrolls:
          s.me().grant_tech(free_tech);
          if (log) log->push_back("Ruin: Scrolls of Wisdom -- a free technology");
          break;
        case kPopulation:
          add_population(s, rules, capital, 3, log);
          if (log) log->push_back("Ruin: Population -- +3 to your capital");
          break;
        case kNewFriends: {
          // A VETERAN Swordsman on land, a veteran Rammer on water. Before 2.0.58 the land
          // reward was a Giant; do not reintroduce that.
          const UnitType gift = on_water ? UnitType::Rammer : UnitType::Swordsman;
          const UnitId id = s.unit_count++;
          Unit& u = s.units[id];
          u = Unit{};
          u.type = gift;
          u.carried_type = gift;
          u.owner = s.current_player;
          u.tile = kNoTile;
          u.hp = to_hp(rules.unit(gift).hp + static_cast<int>(rules.veteran_hp_bonus));
          u.add(UnitFlag::Veteran);
          u.add(UnitFlag::FreshlyTrained);
          // The examining unit still occupies the ruin tile, so place the gift adjacent.
          const Coord c = s.dims.coord(actor_tile);
          for (const Coord off : kNeighbourOffsets) {
            const Coord n{static_cast<std::int16_t>(c.x + off.x),
                          static_cast<std::int16_t>(c.y + off.y)};
            if (!s.dims.contains(n)) continue;
            const TileIdx t = s.dims.index(n);
            if (s.tiles[t].unit == kNoUnit && tribe_can_enter(s, rules, u.owner, s.tiles[t])) {
              u.tile = t;
              s.tiles[t].unit = id;
              break;
            }
          }
          if (u.tile == kNoTile) {
            --s.unit_count;  // nowhere to stand; fall back to stars
            s.me().stars += 10;
            if (log) log->push_back("Ruin: Resources -- +10 stars");
          } else if (log) {
            log->push_back(std::string("Ruin: New Friends -- a veteran ") +
                           (on_water ? "Rammer" : "Swordsman"));
          }
          break;
        }
        case kExplorer:
          // The same event the level-2 city reward runs: reveals a swathe of map, spawns
          // nothing. run_explorer() already existed and was reachable only from a city
          // level-up, never from a ruin.
          run_explorer(s, rules, s.current_player, actor_tile, log);
          if (log) log->push_back("Ruin: Explorer -- a stretch of the map revealed");
          break;
        default:
          s.me().stars += 10;
          if (log) log->push_back("Ruin: Resources -- +10 stars");
          break;
      }

      s.units[s.tiles[actor_tile].unit].add(UnitFlag::TurnEnded);
      break;
    }
    default: break;
  }

  s.phase = Phase::Select;
  s.selected = kNoTile;
  s.global_actor = false;
  return true;
}

// -----------------------------------------------------------------------------------------
// Termination
// -----------------------------------------------------------------------------------------

MaybeOutcome outcome(const GameState& s, const Rules& rules) {
  int alive = 0;
  PlayerId survivor = kNoPlayer;
  for (PlayerId p = 0; p < s.player_count; ++p) {
    if (s.players[p].alive) {
      ++alive;
      survivor = p;
    }
  }

  if (alive <= 1) {
    Outcome result;
    result.how = Termination::LastStanding;
    result.winner = survivor;
    result.turn = s.turn;
    for (PlayerId p = 0; p < s.player_count; ++p) {
      result.score[static_cast<std::size_t>(p)] = player_score(s, rules, p);
    }
    return result;
  }

  // A turn limit reached is a REAL result, distinct from a search horizon cutoff -- which
  // never produces an Outcome at all (outcome.hpp).
  if (s.turn_limit > 0 && s.turn >= s.turn_limit) {
    Outcome result;
    result.how = Termination::ScoreAtTurnLimit;
    result.turn = s.turn;
    int best = -1;
    PlayerId leader = kNoPlayer;
    bool tied = false;
    for (PlayerId p = 0; p < s.player_count; ++p) {
      const auto slot = static_cast<std::size_t>(p);
      result.score[slot] = player_score(s, rules, p);
      if (result.score[slot] > best) {
        best = result.score[slot];
        leader = p;
        tied = false;
      } else if (result.score[slot] == best) {
        tied = true;
      }
    }
    result.winner = tied ? kNoPlayer : leader;
    return result;
  }

  return std::nullopt;
}

// -----------------------------------------------------------------------------------------
// Option enumeration -- the human-facing projection of the same predicates legal_actions uses
// -----------------------------------------------------------------------------------------

namespace {

Option make_option(Verb verb, Layer layer, int cost, Availability status, BlockReason reason) {
  Option o;
  o.verb = verb;
  o.layer = layer;
  o.cost = cost;
  o.status = status;
  o.reason = reason;
  return o;
}

bool keep(Availability status, bool include_unavailable) {
  return include_unavailable || status == Availability::Available;
}

}  // namespace

int tile_layers(const GameState& s, TileIdx tile, Layer* out) {
  int count = 0;
  if (s.tiles[tile].unit != kNoUnit && s.units[s.tiles[tile].unit].alive()) {
    out[count++] = Layer::Unit;
  }
  const CityId city = city_at(s, tile);
  if (city != kNoCity) {
    out[count++] = Layer::City;
    // Deliberately NO Tile layer on a city tile: you cannot build under your own city, and
    // the owner confirmed the real game offers no such layer. A phantom third layer would
    // make every city tile take an extra tap to cycle through nothing.
    return count;
  }
  out[count++] = Layer::Tile;
  return count;
}

void enumerate_options(const GameState& s, const Rules& rules, TileIdx tile, Layer layer,
                       bool include_unavailable, std::vector<Option>& out) {
  out.clear();
  if (tile < 0 || tile >= static_cast<TileIdx>(s.tile_count())) return;
  BlockReason reason = BlockReason::None;

  if (layer == Layer::Unit) {
    const UnitId id = s.tiles[tile].unit;
    if (id == kNoUnit || !s.units[id].alive()) return;
    const Unit& u = s.units[id];
    if (u.owner != s.current_player) return;  // enemy units are inspectable, not commandable

    const UnitStats& stats = rules.unit(u.type);
    const bool dash = has_skill(static_cast<Skill>(stats.skills), Skill::Dash);
    const bool escape = has_skill(static_cast<Skill>(stats.skills), Skill::Escape);

    // Why a unit cannot act is worth SAYING rather than omitting -- "bought this turn" and
    // "already moved" look identical if the option simply disappears.
    BlockReason spent = BlockReason::None;
    if (u.has(UnitFlag::FreshlyTrained)) spent = BlockReason::FreshlyTrained;
    else if (u.has(UnitFlag::TurnEnded)) spent = BlockReason::UnitSpent;
    // A city owing a level-up freezes everything on its tile -- `legal_verbs` returns a mask of
    // {ChooseReward, Deselect} and nothing else. The enumerator did not know that, so a unit
    // standing on such a city was still offered Move, and the UI painted a button apply()
    // then refused. Same rule, stated in both places.
    if (spent == BlockReason::None) {
      const CityId here = city_at(s, tile);
      if (here != kNoCity && s.cities[here].owner == s.current_player &&
          s.cities[here].pending_rewards > 0) {
        spent = BlockReason::PendingReward;
      }
    }

    {
      ActionMask moves;
      if (spent == BlockReason::None) reachable_tiles(s, rules, id, moves);
      const bool can = spent == BlockReason::None && u.moves_left > 0 &&
                       (!u.has(UnitFlag::HasAttacked) || escape) && !moves.empty();
      const auto status = can ? Availability::Available : Availability::Blocked;
      if (keep(status, include_unavailable)) {
        out.push_back(make_option(Verb::Move, Layer::Unit, 0, status,
                                  can ? BlockReason::None
                                      : (spent != BlockReason::None ? spent : BlockReason::OutOfRange)));
      }
    }
    {
      bool target_in_range = false;
      const Coord c = s.dims.coord(u.tile);
      for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()) && !target_in_range; ++t) {
        const UnitId other = s.tiles[t].unit;
        if (other == kNoUnit || !s.units[other].alive() || s.units[other].owner == u.owner) continue;
        const Coord tc = s.dims.coord(t);
        if (std::max(std::abs(c.x - tc.x), std::abs(c.y - tc.y)) <= stats.range) target_in_range = true;
      }
      const bool can = spent == BlockReason::None && !u.has(UnitFlag::HasAttacked) &&
                       (!u.has(UnitFlag::HasMoved) || dash) && stats.attack > 0.0f && target_in_range;
      const auto status = can ? Availability::Available : Availability::Blocked;
      if (keep(status, include_unavailable)) {
        out.push_back(make_option(Verb::Attack, Layer::Unit, 0, status,
                                  can ? BlockReason::None
                                      : (spent != BlockReason::None ? spent : BlockReason::OutOfRange)));
      }
    }
    // Capture and Examine need the unit to have STARTED the turn here (resolutions.md R25).
    const bool started_here = !u.has(UnitFlag::ArrivedThisTurn);
    const CityId city = city_at(s, tile);
    const bool capturable = s.tiles[tile].resource == Resource::Village ||
                            (city != kNoCity && s.cities[city].owner != u.owner);
    if (capturable) {
      const bool can = started_here && spent == BlockReason::None;
      const auto status = can ? Availability::Available : Availability::Blocked;
      if (keep(status, include_unavailable)) {
        // Self-targeting: the actor's own tile IS the target. Leaving this 0 forced the UI
        // to special-case which verbs target themselves, which is engine knowledge leaking
        // into the client.
        Option o = make_option(Verb::Capture, Layer::Unit, 0, status,
                               can ? BlockReason::None : BlockReason::NeedsTurnStart);
        o.action = Action{static_cast<std::int16_t>(tile_to_slot(tile))};
        out.push_back(o);
      }
    }
    if (s.tiles[tile].resource == Resource::Ruin) {
      const bool can = started_here && spent == BlockReason::None;
      const auto status = can ? Availability::Available : Availability::Blocked;
      if (keep(status, include_unavailable)) {
        Option o = make_option(Verb::Examine, Layer::Unit, 0, status,
                               can ? BlockReason::None : BlockReason::NeedsTurnStart);
        o.action = Action{static_cast<std::int16_t>(tile_to_slot(tile))};
        out.push_back(o);
      }
    }

    // Recover -- offered on a damaged unit that has not attacked or already healed. See
    // may_recover(): moving does NOT take this away.
    if (u.hp < max_hp_for(rules, u)) {
      const bool can = may_recover(s, rules, id);
      const auto status = can ? Availability::Available : Availability::Blocked;
      if (keep(status, include_unavailable)) {
        Option o = make_option(Verb::Recover, Layer::Unit, 0, status,
                               can ? BlockReason::None : spent);
        o.action = Action{static_cast<std::int16_t>(tile_to_slot(tile))};
        out.push_back(o);
      }
    }

    // Promote -- BANKED, so it stays on offer indefinitely once earned. Deliberately not
    // automatic: promotion heals completely, so holding it is a free heal in reserve.
    const bool promotable = !has_skill(static_cast<Skill>(stats.skills), Skill::Static) &&
                            !u.has(UnitFlag::Veteran);
    if (promotable && (u.kills > 0 || include_unavailable)) {
      const bool can = u.kills >= rules.veteran_kills_required;
      const auto status = can ? Availability::Available : Availability::Blocked;
      if (keep(status, include_unavailable)) {
        Option o = make_option(Verb::Promote, Layer::Unit, 0, status,
                               can ? BlockReason::None : BlockReason::OutOfRange);
        o.action = Action{static_cast<std::int16_t>(tile_to_slot(tile))};
        out.push_back(o);
      }
    }

    if (has_skill(static_cast<Skill>(stats.skills), Skill::Heal)) {
      const bool can = spent == BlockReason::None;
      const auto status = can ? Availability::Available : Availability::Blocked;
      if (keep(status, include_unavailable)) {
        Option o = make_option(Verb::Heal, Layer::Unit, 0, status,
                               can ? BlockReason::None : spent);
        o.action = Action{static_cast<std::int16_t>(tile_to_slot(tile))};
        out.push_back(o);
      }
    }

    if (has_skill(static_cast<Skill>(stats.skills), Skill::Convert)) {
      bool adjacent_enemy = false;
      const Coord c = s.dims.coord(tile);
      for (const Coord off : kNeighbourOffsets) {
        const Coord n{static_cast<std::int16_t>(c.x + off.x),
                      static_cast<std::int16_t>(c.y + off.y)};
        if (!s.dims.contains(n)) continue;
        const UnitId other = s.tiles[s.dims.index(n)].unit;
        if (other != kNoUnit && s.units[other].alive() && s.units[other].owner != u.owner) {
          adjacent_enemy = true;
        }
      }
      const bool can = spent == BlockReason::None && adjacent_enemy;
      const auto status = can ? Availability::Available : Availability::Blocked;
      if (keep(status, include_unavailable)) {
        out.push_back(make_option(Verb::Convert, Layer::Unit, 0, status,
                                  can ? BlockReason::None : BlockReason::OutOfRange));
      }
    }

    // Embark / naval upgrade.
    {
      BlockReason why = BlockReason::None;
      const Availability status = can_embark(s, rules, tile, why);
      if (status != Availability::Blocked && keep(status, include_unavailable)) {
        Option o = make_option(Verb::Move, Layer::Unit, 0, status, why);
        o.action = Action{static_cast<std::int16_t>(tile_to_slot(tile))};
        (void)o;
      }
      for (const UnitType into : {UnitType::Scout, UnitType::Rammer, UnitType::Bomber}) {
        const Availability up = can_upgrade_naval(s, rules, tile, into, why);
        if (up == Availability::Blocked || !keep(up, include_unavailable)) continue;
        Option o = make_option(Verb::UpgradeNaval, Layer::Unit, rules.unit(into).cost, up, why);
        o.unit_type = into;
        o.required_tech = rules.unit(into).tech;
        o.action = Action{static_cast<std::int16_t>(unit_type_to_slot(into))};
        out.push_back(o);
      }
    }

    {
      const bool can = spent == BlockReason::None;
      const auto status = can ? Availability::Available : Availability::Blocked;
      if (keep(status, include_unavailable)) {
        Option o = make_option(Verb::Disband, Layer::Unit, -(rules.unit(u.type).cost / 2),
                               status, can ? BlockReason::None : spent);
        o.action = Action{static_cast<std::int16_t>(tile_to_slot(tile))};
        out.push_back(o);
      }
    }
    return;
  }

  if (layer == Layer::City) {
    const CityId city = city_at(s, tile);
    if (city == kNoCity) return;
    const City& c = s.cities[city];
    if (c.owner != s.current_player) return;

    if (c.pending_rewards > 0) {
      // A pending level-up blocks everything else, so offer only the choice.
      const int level = reward_menu_level(c);
      CityReward a = CityReward::Park;
      CityReward b = CityReward::SuperUnit;
      if (level == 2) { a = CityReward::Workshop; b = CityReward::Explorer; }
      else if (level == 3) { a = CityReward::CityWall; b = CityReward::Resources; }
      else if (level == 4) { a = CityReward::PopulationGrowth; b = CityReward::BorderGrowth; }
      for (const CityReward r : {a, b}) {
        Option o = make_option(Verb::ChooseReward, Layer::City, 0, Availability::Available,
                               BlockReason::None);
        o.reward = r;
        o.action = Action{static_cast<std::int16_t>(reward_to_slot(r))};
        out.push_back(o);
      }
      return;
    }

    for (int i = 0; i < kUnitTypeCount; ++i) {
      const auto type = static_cast<UnitType>(i);
      if (!rules.unit(type).trainable) continue;
      const Availability status = can_train(s, rules, city, type, reason);
      if (!keep(status, include_unavailable)) continue;
      Option o = make_option(Verb::Train, Layer::City, rules.unit(type).cost, status, reason);
      o.unit_type = type;
      o.required_tech = rules.unit(type).tech;
      o.action = Action{static_cast<std::int16_t>(unit_type_to_slot(type))};
      out.push_back(o);
    }
    return;
  }

  // Layer::Tile -- build and harvest.
  for (int i = 1; i < static_cast<int>(Improvement::Count); ++i) {
    const auto imp = static_cast<Improvement>(i);
    const Availability status = can_build(s, rules, tile, imp, reason);
    // Wrong terrain is not an interesting refusal -- listing every building on every tile
    // would bury the ones that matter. Everything else is worth saying out loud.
    if (status == Availability::Blocked &&
        (reason == BlockReason::WrongTerrain || reason == BlockReason::NotYourTerritory ||
         reason == BlockReason::NoResource)) {
      continue;
    }
    if (!keep(status, include_unavailable)) continue;
    Option o = make_option(Verb::Build, Layer::Tile, rules.improvement(imp).cost, status, reason);
    o.improvement = imp;
    o.required_tech = rules.improvement(imp).tech;
    o.action = Action{static_cast<std::int16_t>(improvement_to_slot(imp))};
    out.push_back(o);
  }

  if (s.tiles[tile].improvement != Improvement::None &&
      s.tiles[tile].owner == s.current_player) {
    Option o = make_option(Verb::Destroy, Layer::Tile, 0, Availability::Available,
                           BlockReason::None);
    o.action = Action{static_cast<std::int16_t>(tile_to_slot(tile))};
    out.push_back(o);
  }

  const Availability harvest = can_harvest(s, rules, tile, reason);
  if (harvest != Availability::Blocked && keep(harvest, include_unavailable)) {
    Option o = make_option(Verb::Harvest, Layer::Tile,
                           rules.resource(s.tiles[tile].resource).harvest_cost, harvest, reason);
    o.required_tech = rules.resource(s.tiles[tile].resource).tech;
    o.action = Action{static_cast<std::int16_t>(tile_to_slot(tile))};
    out.push_back(o);
  }
}

void enumerate_research(const GameState& s, const Rules& rules, bool include_unavailable,
                        std::vector<Option>& out) {
  out.clear();
  BlockReason reason = BlockReason::None;
  int cities = 0;
  for (CityId c = 0; c < s.city_count; ++c) {
    if (s.cities[c].owner == s.current_player) ++cities;
  }
  for (int i = 0; i < kTechCount; ++i) {
    const auto tech = static_cast<Tech>(i);
    const Availability status = can_research(s, rules, s.current_player, tech, reason);
    if (status == Availability::Blocked) continue;  // already researched
    if (!keep(status, include_unavailable)) continue;
    Option o = make_option(Verb::Research, Layer::Tile,
                           rules.tech_cost(tech, cities, s.me().has_tech(Tech::Philosophy)),
                           status, reason);
    o.tech = tech;
    o.required_tech = tech_is_root(tech) ? tech : tech_parent(tech);
    o.action = Action{static_cast<std::int16_t>(tech_to_slot(tech))};
    out.push_back(o);
  }
}

bool has_any_available_action(const GameState& s, const Rules& rules) {
  // "Nothing left to do" means NOTHING -- not merely "no unit can move". A player with stars
  // for a building, a harvest, a unit or a technology still has a turn to take, so lighting
  // End Turn on unit-exhaustion alone would rush them past real decisions.
  std::vector<Option> options;
  for (CityId c = 0; c < s.city_count; ++c) {
    if (s.cities[c].owner != s.current_player) continue;
    if (s.cities[c].pending_rewards > 0) return true;
  }
  for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
    Layer layers[4];
    const int count = tile_layers(s, t, layers);
    for (int i = 0; i < count; ++i) {
      enumerate_options(s, rules, t, layers[i], false, options);
      if (!options.empty()) return true;
    }
  }
  enumerate_research(s, rules, false, options);
  return !options.empty();
}

// The city holding an unresolved level-up, or kNoCity. A level-up must be answered
// IMMEDIATELY -- the real game makes it a true modal, and the engine already refuses every
// other verb while one is outstanding, so the UI has to surface it rather than let the player
// wander off.
int tile_population_yield(const GameState& s, const Rules& rules, TileIdx tile) {
  return tile_population_yield_impl(s, rules, tile);
}

void capture_city_for_test(GameState& s, const Rules& rules, CityId city, PlayerId new_owner,
                           std::vector<std::string>* log) {
  capture_city(s, rules, city, new_owner, log);
}

CityId pending_reward_city(const GameState& s) {
  for (CityId c = 0; c < s.city_count; ++c) {
    if (s.cities[c].owner == s.current_player && s.cities[c].pending_rewards > 0) return c;
  }
  return kNoCity;
}

CombatPreview preview_combat(const GameState& s, const Rules& rules, TileIdx from, TileIdx to) {
  CombatPreview preview;
  if (from < 0 || to < 0 || from >= static_cast<TileIdx>(s.tile_count()) ||
      to >= static_cast<TileIdx>(s.tile_count())) {
    return preview;
  }
  const UnitId attacker = s.tiles[from].unit;
  const UnitId defender = s.tiles[to].unit;
  if (attacker == kNoUnit || defender == kNoUnit) return preview;
  if (!s.units[attacker].alive() || !s.units[defender].alive()) return preview;
  if (s.units[attacker].owner == s.units[defender].owner) return preview;

  // `valid` means THIS ATTACK IS LEGAL RIGHT NOW, not merely "two hostile units".
  //
  // The UI paints its red attack rings from this answer, exactly as it paints cyan move
  // rings from reachable_tiles(). If `valid` only meant "these are enemies" the UI would
  // have to re-derive weapon range and the spent/has-attacked rules client-side -- a second
  // implementation of the rules, which is the one thing this project cannot have
  // (README.md §4). So the same predicate enumerate_options() uses for Verb::Attack is
  // applied here, and the two cannot drift because both read these flags.
  const Unit& a = s.units[attacker];
  if (a.owner != s.current_player) return preview;
  if (a.has(UnitFlag::TurnEnded) || a.has(UnitFlag::FreshlyTrained)) return preview;
  if (a.has(UnitFlag::HasAttacked)) return preview;

  const UnitStats& astats = rules.unit(a.type);
  if (astats.attack <= 0.0f) return preview;
  const bool dash = has_skill(static_cast<Skill>(astats.skills), Skill::Dash);
  if (a.has(UnitFlag::HasMoved) && !dash) return preview;

  const Coord ac = s.dims.coord(from);
  const Coord dc = s.dims.coord(to);
  if (std::max(std::abs(ac.x - dc.x), std::abs(ac.y - dc.y)) > astats.range) return preview;

  const CombatResult result = resolve_combat(s, rules, attacker, defender);
  preview.valid = true;
  preview.attacker_damage = result.attacker_damage / kHpScale;
  preview.defender_damage = result.defender_damage / kHpScale;
  preview.defender_dies = result.defender_dies;
  preview.attacker_dies = result.retaliates && result.defender_damage >= s.units[attacker].hp;
  return preview;
}

// Drives the phase machine through one complete human-scale action. The UI thinks in
// "build a Farm here"; the agent thinks in three micro-decisions. Both go through apply(),
// so there is exactly one implementation of what an action DOES -- this only bundles the
// keystrokes. Any step failing rolls the whole thing back.
bool apply_compound(GameState& s, const Rules& rules, TileIdx tile, Verb verb, int target_slot,
                    std::vector<std::string>* log) {
  const GameState snapshot = s;
  const bool global = tile == kNoTile;
  const int select_slot = global ? kGlobalActorSlot : tile_to_slot(tile);

  if (s.phase != Phase::Select) {
    if (!apply(s, rules, Action{static_cast<std::int16_t>(Verb::Deselect)}, nullptr)) {
      s = snapshot;
      return false;
    }
  }
  if (!apply(s, rules, Action{static_cast<std::int16_t>(select_slot)}, log) ||
      !apply(s, rules, Action{static_cast<std::int16_t>(verb)}, log)) {
    s = snapshot;
    return false;
  }
  if (verb == Verb::EndTurn || verb == Verb::Deselect) return true;
  if (!apply(s, rules, Action{static_cast<std::int16_t>(target_slot)}, log)) {
    s = snapshot;
    return false;
  }
  return true;
}

}  // namespace poly
