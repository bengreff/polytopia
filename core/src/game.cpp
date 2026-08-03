#include "poly/game.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>

#include "poly/json.hpp"
#include "poly/mapgen.hpp"

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
    case Terrain::Water:
    case Terrain::Ocean: return false;  // land units need a hull; naval is a later milestone
    default: return true;
  }
  (void)rules;
}

int unit_max_hp(const Rules& rules, const Unit& u) {
  const UnitStats& stats = rules.unit(u.has(UnitFlag::Embarked) ? u.carried_type : u.type);
  int hp = stats.hp;
  if (u.has(UnitFlag::Veteran)) hp += static_cast<int>(rules.veteran_hp_bonus);
  return hp;
}

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
    // Standing on a mountain grants +1 vision (resolutions.md R18).
    if (s.tiles[unit.tile].terrain == Terrain::Mountain) radius += rules.mountain_vision_bonus;
    reveal_around(unit.tile, radius);
  }
  // Territory reveals fog.
  for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
    if (s.tiles[t].owner == player) seen.set(t);
  }
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

namespace {

void begin_turn(GameState& s, const Rules& rules) {
  const PlayerId player = s.current_player;
  update_siege_flags(s);

  // Income at the START of the turn (resolutions.md R1), so a city captured last turn is
  // spendable this turn.
  if (rules.income_at_turn_start) {
    s.players[player].stars += player_income(s, rules, player);
  }

  for (UnitId u = 0; u < s.unit_count; ++u) {
    Unit& unit = s.units[u];
    if (unit.owner != player || !unit.alive()) continue;
    unit.moves_left = static_cast<std::uint8_t>(rules.unit(unit.type).move);
    unit.remove(UnitFlag::HasMoved);
    unit.remove(UnitFlag::HasAttacked);
    unit.remove(UnitFlag::TurnEnded);
    unit.remove(UnitFlag::ArrivedThisTurn);
    // Clears here, not on spawn: the unit becomes usable on the owner's NEXT turn.
    unit.remove(UnitFlag::FreshlyTrained);
  }

  // Temples grow every 2 turns to level 5 -- owner-verified, and contradicting every wiki
  // source, which said 12 turns to max (resolutions.md R24).
  if (rules.temple_growth_interval > 0 && s.turn % rules.temple_growth_interval == 0) {
    for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
      Tile& tile = s.tiles[t];
      if (tile.owner != player) continue;
      const bool is_temple = tile.improvement == Improvement::Temple ||
                             tile.improvement == Improvement::ForestTemple ||
                             tile.improvement == Improvement::MountainTemple ||
                             tile.improvement == Improvement::WaterTemple;
      (void)is_temple;  // temple levels are tracked in a later milestone
    }
  }

  recompute_visibility(s, rules, player);
  s.phase = Phase::Select;
  s.selected = kNoTile;
  s.global_actor = false;
}

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

bool can_train(const GameState& s, const Rules& rules, CityId city_id, UnitType type) {
  const City& c = s.cities[city_id];
  const UnitStats& stats = rules.unit(type);
  if (!stats.trainable) return false;
  if (c.unit_count >= c.unit_capacity()) return false;
  if (s.units[0].owner == kNoPlayer && s.unit_count >= kMaxUnits) return false;
  if (s.unit_count >= kMaxUnits) return false;
  if (stats.needs_tech && !s.players[c.owner].has_tech(stats.tech)) return false;
  if (s.players[c.owner].stars < stats.cost) return false;
  if (s.tiles[c.tile].unit != kNoUnit) return false;
  return true;
}

bool can_research(const GameState& s, const Rules& rules, PlayerId player, Tech tech) {
  const Player& p = s.players[player];
  if (p.has_tech(tech)) return false;
  // Backwards research: the edge is traversable in BOTH directions, so a tech is available
  // if its parent is owned OR any child of it is owned (resolutions.md R30).
  bool available = tech_is_root(tech) || p.has_tech(tech_parent(tech));
  if (!available) {
    for (int i = 0; i < kTechCount && !available; ++i) {
      const auto other = static_cast<Tech>(i);
      if (!tech_is_root(other) && tech_parent(other) == tech && p.has_tech(other)) {
        available = true;
      }
    }
  }
  if (!available) return false;

  int city_count = 0;
  for (CityId c = 0; c < s.city_count; ++c) {
    if (s.cities[c].owner == player) ++city_count;
  }
  return p.stars >= rules.tech_cost(tech, city_count, p.has_tech(Tech::Philosophy));
}

bool can_build(const GameState& s, const Rules& rules, TileIdx tile_idx, Improvement imp) {
  const Tile& tile = s.tiles[tile_idx];
  const ImprovementStats& stats = rules.improvement(imp);
  if (imp == Improvement::None) return false;
  if (tile.improvement != Improvement::None) return false;
  if (tile.owner != s.current_player) return false;
  if ((stats.terrain_mask & terrain_bit(tile.terrain)) == 0) return false;
  if (stats.needs_resource && tile.resource != stats.resource) return false;
  if (stats.needs_tech && !s.me().has_tech(stats.tech)) return false;
  if (s.me().stars < stats.cost) return false;

  if (stats.per_city_limit > 0 && tile.city != kNoCity) {
    int existing = 0;
    for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
      if (s.tiles[t].city == tile.city && s.tiles[t].improvement == imp) ++existing;
    }
    if (existing >= stats.per_city_limit) return false;
  }
  return true;
}

bool can_harvest(const GameState& s, const Rules& rules, TileIdx tile_idx) {
  const Tile& tile = s.tiles[tile_idx];
  if (tile.owner != s.current_player) return false;
  const ResourceStats& stats = rules.resource(tile.resource);
  if (tile.resource == Resource::None || !stats.harvestable) return false;
  if (stats.needs_tech && !s.me().has_tech(stats.tech)) return false;
  return s.me().stars >= stats.harvest_cost;
}

void legal_select(const GameState& s, const Rules& rules, ActionMask& mask) {
  for (UnitId u = 0; u < s.unit_count; ++u) {
    if (unit_can_act(s, u)) mask.set(tile_to_slot(s.units[u].tile));
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
      if (can_train(s, rules, c, static_cast<UnitType>(i))) {
        mask.set(tile_to_slot(city.tile));
        break;
      }
    }
  }
  for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
    if (can_harvest(s, rules, t)) {
      mask.set(tile_to_slot(t));
      continue;
    }
    for (int i = 1; i < static_cast<int>(Improvement::Count); ++i) {
      if (can_build(s, rules, t, static_cast<Improvement>(i))) {
        mask.set(tile_to_slot(t));
        break;
      }
    }
  }
  // The global pseudo-actor carries research and end-turn.
  mask.set(kGlobalActorSlot);
}

void legal_verbs(const GameState& s, const Rules& rules, ActionMask& mask) {
  if (s.global_actor) {
    for (int i = 0; i < kTechCount; ++i) {
      if (can_research(s, rules, s.current_player, static_cast<Tech>(i))) {
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
      if (can_train(s, rules, city_id, static_cast<UnitType>(i))) {
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

  if (can_harvest(s, rules, tile_idx)) mask.set(static_cast<int>(Verb::Harvest));
  for (int i = 1; i < static_cast<int>(Improvement::Count); ++i) {
    if (can_build(s, rules, tile_idx, static_cast<Improvement>(i))) {
      mask.set(static_cast<int>(Verb::Build));
      break;
    }
  }

  mask.set(static_cast<int>(Verb::Deselect));
}

void legal_targets(const GameState& s, const Rules& rules, ActionMask& mask) {
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
        if (can_train(s, rules, city_id, static_cast<UnitType>(i))) {
          mask.set(unit_type_to_slot(static_cast<UnitType>(i)));
        }
      }
      break;
    }
    case Verb::Research: {
      for (int i = 0; i < kTechCount; ++i) {
        if (can_research(s, rules, s.current_player, static_cast<Tech>(i))) {
          mask.set(tech_to_slot(static_cast<Tech>(i)));
        }
      }
      break;
    }
    case Verb::Build: {
      for (int i = 1; i < static_cast<int>(Improvement::Count); ++i) {
        if (can_build(s, rules, tile_idx, static_cast<Improvement>(i))) {
          mask.set(improvement_to_slot(static_cast<Improvement>(i)));
        }
      }
      break;
    }
    case Verb::ChooseReward: {
      const CityId city_id = city_at(s, tile_idx);
      const int level = s.cities[city_id].level;
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

void capture_city(GameState& s, const Rules& rules, CityId city_id, PlayerId new_owner,
                  std::vector<std::string>* log) {
  City& c = s.cities[city_id];
  const PlayerId old_owner = c.owner;
  c.owner = new_owner;

  // Territory transfers with the city, keeping its Border Growth (resolutions.md R16).
  for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
    if (s.tiles[t].city == city_id) s.tiles[t].owner = new_owner;
  }
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
      claim_territory(s, city_id, rules.grown_city_border);
      break;
    case CityReward::Park: c.park_count = static_cast<std::uint8_t>(c.park_count + 1); break;
    case CityReward::SuperUnit:
    case CityReward::Explorer:
    default:
      // Spawning displaces whatever occupies the city (resolutions.md R27) -- implemented
      // with the rest of unit spawning in the next milestone.
      break;
  }
  c.pending_rewards = static_cast<std::int8_t>(c.pending_rewards - 1);
}

}  // namespace

bool apply(GameState& s, const Rules& rules, Action action, std::vector<std::string>* log) {
  ActionMask mask;
  legal_actions(s, rules, mask);
  if (action.index < 0 || action.index >= kActionSlots || !mask.test(action.index)) return false;

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
      u.add(UnitFlag::HasMoved);
      u.add(UnitFlag::ArrivedThisTurn);
      u.moves_left = 0;
      // Moving ends the turn unless the unit has Dash (resolutions.md R26).
      if (!has_skill(static_cast<Skill>(rules.unit(u.type).skills), Skill::Dash) ||
          u.has(UnitFlag::HasAttacked)) {
        u.add(UnitFlag::TurnEnded);
      }
      recompute_visibility(s, rules, u.owner);
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
      if (!s.units[defender].alive()) kill_unit(s, defender);
      Unit& a = s.units[attacker];
      if (!a.alive()) {
        kill_unit(s, attacker);
      } else {
        a.add(UnitFlag::HasAttacked);
        // Attacking ends the turn unless the unit has Escape (R26).
        if (!has_skill(static_cast<Skill>(rules.unit(a.type).skills), Skill::Escape)) {
          a.add(UnitFlag::TurnEnded);
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
        claim_territory(s, city_id, rules.new_city_border);
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
      const ImprovementStats& stats = rules.improvement(imp);
      s.me().stars -= stats.cost;
      tile.improvement = imp;
      int population = stats.population;
      // Adjacency buildings score off neighbours rather than carrying a flat yield.
      if (stats.adjacency_source != Improvement::None) {
        population = stats.adjacency_population *
                     count_adjacent_improvements(s, actor_tile, stats.adjacency_source,
                                                 s.current_player);
      }
      if (tile.city != kNoCity) add_population(s, rules, tile.city, population, log);
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
    case Verb::Examine: {
      Tile& tile = s.tiles[actor_tile];
      tile.resource = Resource::None;
      // Uniform reward selection, drawn from the STATE's rng so replays reproduce
      // (UNRESOLVED #17: the wiki says uniform, v2.16.3 says the calculation changed).
      s.me().stars += 5 + static_cast<int>(s.rng.bounded(6));
      s.units[s.tiles[actor_tile].unit].add(UnitFlag::TurnEnded);
      if (log) log->push_back("Explored a ruin");
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

}  // namespace poly
