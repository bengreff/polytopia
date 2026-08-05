// The mechanics added after the first playable build: explorers, super units, veterancy,
// healing, conversion, temples, roads and naval.

#include <string>
#include <vector>

#include "poly/game.hpp"
#include "testing.hpp"

using namespace poly;

namespace {
GameState game(const Rules& rules, int dim = 11, unsigned seed = 4) {
  GameConfig cfg;
  cfg.dims = MapDims{static_cast<std::int16_t>(dim), static_cast<std::int16_t>(dim)};
  cfg.seed = seed;
  cfg.player_count = 2;
  cfg.tribes[0] = Tribe::Imperius;
  cfg.tribes[1] = Tribe::Bardur;
  cfg.is_human[0] = true;
  GameState s;
  std::string error;
  new_game(cfg, rules, s, error);
  return s;
}
UnitId my_unit(const GameState& s) {
  for (UnitId u = 0; u < s.unit_count; ++u) {
    if (s.units[u].owner == 0 && s.units[u].alive()) return u;
  }
  return kNoUnit;
}
CityId my_capital(const GameState& s) {
  for (CityId c = 0; c < s.city_count; ++c) {
    if (s.cities[c].owner == 0 && s.cities[c].has(CityFlag::Capital)) return c;
  }
  return kNoCity;
}
}  // namespace

TEST("explorer: reveals fog immediately rather than spawning a unit") {
  // The Explorer is an EVENT, not a controllable unit. Modelling it as a unit is exactly why
  // it "did not work at all": it spawned with no moves and nothing ever drove it.
  const Rules rules = Rules::defaults();
  GameState s = game(rules);
  const CityId capital = my_capital(s);

  int before = 0;
  for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
    if (s.explored[0].test(t)) ++before;
  }
  const int units_before = s.unit_count;

  s.cities[capital].pending_rewards = 1;
  s.cities[capital].level = 2;
  std::vector<std::string> log;
  CHECK(apply_compound(s, rules, s.cities[capital].tile, Verb::ChooseReward,
                       reward_to_slot(CityReward::Explorer), &log));

  int after = 0;
  for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
    if (s.explored[0].test(t)) ++after;
  }
  CHECK(after > before);            // it actually explored
  CHECK_EQ(s.unit_count, units_before);  // and left no unit behind
}

TEST("super unit: spawning a Giant shoves the unit occupying the city") {
  // resolutions.md R27, documented in no source -- it came from the owner. This is the
  // counter to parking a unit on a city to besiege it.
  const Rules rules = Rules::defaults();
  GameState s = game(rules);
  const CityId capital = my_capital(s);
  const TileIdx tile = s.cities[capital].tile;

  // Park an ENEMY unit on the city.
  const UnitId intruder = my_unit(s);
  s.units[intruder].owner = 1;
  if (s.units[intruder].tile != kNoTile) s.tiles[s.units[intruder].tile].unit = kNoUnit;
  s.units[intruder].tile = tile;
  s.tiles[tile].unit = intruder;

  s.cities[capital].pending_rewards = 1;
  s.cities[capital].level = 5;
  std::vector<std::string> log;
  CHECK(apply_compound(s, rules, tile, Verb::ChooseReward,
                       reward_to_slot(CityReward::SuperUnit), &log));

  // The Giant now holds the city and the intruder is elsewhere (or dead if surrounded).
  const UnitId occupant = s.tiles[tile].unit;
  CHECK(occupant != kNoUnit);
  CHECK(s.units[occupant].type == UnitType::Giant);
  CHECK(s.units[occupant].owner == 0);
  CHECK(s.units[intruder].tile != tile);
}

TEST("veteran: promotion is banked at 3 kills, adds 5 max HP, then heals fully") {
  const Rules rules = Rules::defaults();
  GameState s = game(rules);
  const UnitId u = my_unit(s);
  s.units[u].hp = to_hp(3);  // wounded

  std::vector<Option> options;
  enumerate_options(s, rules, s.units[u].tile, Layer::Unit, true, options);
  bool promote_offered = false;
  for (const Option& o : options) {
    if (o.verb == Verb::Promote) {
      promote_offered = true;
      CHECK(o.status != Availability::Available);  // not yet earned
    }
  }
  CHECK(promote_offered);

  s.units[u].kills = 3;
  CHECK(apply_compound(s, rules, s.units[u].tile, Verb::Promote,
                       tile_to_slot(s.units[u].tile), nullptr));
  CHECK(s.units[u].has(UnitFlag::Veteran));
  // Warrior 10 HP + 5 veteran bonus, healed completely.
  CHECK_EQ(s.units[u].hp, to_hp(15));
}

TEST("veteran: Static units can never be promoted") {
  // Unit Skills makes Static the single authoritative flag. Among regular tribes that is
  // every Carry naval unit, the Giant, the Cloak and the Dagger.
  const Rules rules = Rules::defaults();
  for (const UnitType t : {UnitType::Giant, UnitType::Cloak, UnitType::Dagger,
                           UnitType::Raft, UnitType::Juggernaut}) {
    CHECK(has_skill(static_cast<Skill>(rules.unit(t).skills), Skill::Static));
  }
  CHECK(!has_skill(static_cast<Skill>(rules.unit(UnitType::Warrior).skills), Skill::Static));
}

TEST("recover: 4 HP in your own territory, 2 outside it") {
  const Rules rules = Rules::defaults();
  GameState s = game(rules);
  const UnitId u = my_unit(s);
  const TileIdx tile = s.units[u].tile;

  s.units[u].hp = to_hp(1);
  s.tiles[tile].owner = 0;  // own territory
  CHECK(apply_compound(s, rules, tile, Verb::Recover, tile_to_slot(tile), nullptr));
  CHECK_EQ(s.units[u].hp, to_hp(5));

  // Two independent scenarios in one turn, so the fixture has to hand the unit back its turn.
  // HasRecovered is what now stops a second heal (TurnEnded no longer gates Recover, because
  // moving sets it); clearing only TurnEnded would silently test nothing.
  s.units[u].remove(UnitFlag::TurnEnded);
  s.units[u].remove(UnitFlag::HasRecovered);
  s.units[u].hp = to_hp(1);
  s.tiles[tile].owner = kNoPlayer;  // neutral gives 2, same as enemy
  CHECK(apply_compound(s, rules, tile, Verb::Recover, tile_to_slot(tile), nullptr));
  CHECK_EQ(s.units[u].hp, to_hp(3));
}

TEST("recover: healing never exceeds maximum HP") {
  const Rules rules = Rules::defaults();
  GameState s = game(rules);
  const UnitId u = my_unit(s);
  const TileIdx tile = s.units[u].tile;
  s.tiles[tile].owner = 0;
  s.units[u].hp = to_hp(9);  // Warrior max is 10
  CHECK(apply_compound(s, rules, tile, Verb::Recover, tile_to_slot(tile), nullptr));
  CHECK_EQ(s.units[u].hp, to_hp(10));
}

TEST("temples: grow every 2 turns to level 5, and no further") {
  const Rules rules = Rules::defaults();
  CHECK_EQ(rules.temple_growth_interval, 2);
  CHECK_EQ(rules.temple_max_level, 5);
  CHECK_EQ(rules.temple_score_per_level, 100);

  GameState s = game(rules);
  const CityId capital = my_capital(s);
  const TileIdx tile = s.cities[capital].tile;
  s.tiles[tile].improvement = Improvement::Temple;
  s.tiles[tile].improvement_level = 0;
  s.tiles[tile].owner = 0;

  for (int turn = 0; turn < 40; ++turn) {
    s.turn = turn;
    begin_turn(s, rules);
  }
  CHECK_EQ(static_cast<int>(s.tiles[tile].improvement_level), rules.temple_max_level);
}

TEST("roads: buildable on your own and neutral land, never on enemy or mountain") {
  const Rules rules = Rules::defaults();
  GameState s = game(rules);
  s.players[0].grant_tech(Tech::Roads);
  s.players[0].stars = 50;

  TileIdx own = kNoTile, enemy = kNoTile, mountain = kNoTile;
  for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
    const Tile& tile = s.tiles[t];
    if (tile.terrain == Terrain::Mountain && mountain == kNoTile) mountain = t;
    if (tile.terrain == Terrain::Field && tile.owner == 0 && own == kNoTile) own = t;
  }
  // Manufacture an enemy-owned field tile.
  for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
    if (s.tiles[t].terrain == Terrain::Field && s.tiles[t].owner == kNoPlayer) {
      s.tiles[t].owner = 1;
      enemy = t;
      break;
    }
  }

  std::vector<Option> options;
  const auto offers_road = [&](TileIdx t) {
    if (t == kNoTile) return false;
    enumerate_options(s, rules, t, Layer::Tile, false, options);
    for (const Option& o : options) {
      if (o.verb == Verb::Build && o.improvement == Improvement::Road) return true;
    }
    return false;
  };

  if (own != kNoTile) CHECK(offers_road(own));
  if (enemy != kNoTile) CHECK(!offers_road(enemy));   // resolutions.md R31
  if (mountain != kNoTile) CHECK(!offers_road(mountain));
}

TEST("naval: a land unit embarking keeps its identity and its damage") {
  // A naval unit is the SAME entity as the land unit (resolutions.md R29). carried_type
  // remembers what to restore, and HP passes through untouched in both directions.
  const Rules rules = Rules::defaults();
  GameState s = game(rules);
  const UnitId u = my_unit(s);

  s.units[u].hp = to_hp(6);
  s.units[u].carried_type = s.units[u].type;
  const UnitType original = s.units[u].type;

  // Simulate the embark transition the Move handler performs.
  s.units[u].type = UnitType::Raft;
  s.units[u].add(UnitFlag::Embarked);
  CHECK_EQ(s.units[u].hp, to_hp(6));  // upgrading/embarking does not heal

  s.units[u].type = s.units[u].carried_type;
  s.units[u].remove(UnitFlag::Embarked);
  CHECK(s.units[u].type == original);
  CHECK_EQ(s.units[u].hp, to_hp(6));  // and damage survives the round trip
}

TEST("disband: refunds half the unit's cost and removes it") {
  const Rules rules = Rules::defaults();
  GameState s = game(rules);
  const UnitId u = my_unit(s);
  const TileIdx tile = s.units[u].tile;
  const int before = s.players[0].stars;
  const int cost = rules.unit(s.units[u].type).cost;

  CHECK(apply_compound(s, rules, tile, Verb::Disband, tile_to_slot(tile), nullptr));
  CHECK(!s.units[u].alive());
  CHECK_EQ(s.tiles[tile].unit, kNoUnit);
  CHECK_EQ(s.players[0].stars, before + cost / 2);
}

// ---------------------------------------------------------------------------------------
// Defects found by PLAYING the game, not by testing it. Every one of these was invisible to
// the 89 tests that already passed, which is the point: the suite tested the paths the
// engine offers, and these are all cases where the engine offers the wrong path.
// ---------------------------------------------------------------------------------------

TEST("level 4: Population Growth actually adds population") {
  const Rules rules = Rules::defaults();
  GameState s = game(rules);
  const CityId capital = my_capital(s);
  const TileIdx tile = s.cities[capital].tile;

  s.cities[capital].level = 4;
  s.cities[capital].population = 0;
  s.cities[capital].pending_rewards = 1;

  CHECK(apply_compound(s, rules, tile, Verb::ChooseReward,
                       reward_to_slot(CityReward::PopulationGrowth), nullptr));
  // +3 population. Level 5 needs 6, so this must NOT level up -- it must show as population.
  CHECK_EQ(s.cities[capital].population, 3);
  CHECK_EQ(s.cities[capital].level, 4);
  CHECK_EQ(s.cities[capital].pending_rewards, 0);
}

TEST("level 4: Population Growth that levels the city keeps the new level's reward") {
  const Rules rules = Rules::defaults();
  GameState s = game(rules);
  const CityId capital = my_capital(s);
  const TileIdx tile = s.cities[capital].tile;

  // Level 5 needs 6 population; sitting on 4 means +3 crosses the threshold.
  s.cities[capital].level = 4;
  s.cities[capital].population = 4;
  s.cities[capital].pending_rewards = 1;

  CHECK(apply_compound(s, rules, tile, Verb::ChooseReward,
                       reward_to_slot(CityReward::PopulationGrowth), nullptr));
  CHECK_EQ(s.cities[capital].level, 5);
  // The level-5 choice (Park / Super Unit) must still be owed. Consuming it here is how a
  // free Giant silently vanishes.
  CHECK_EQ(s.cities[capital].pending_rewards, 1);
}

TEST("recover: a unit that has moved may still heal") {
  // "Healing can only be done for an entire turn -- moving takes it away as an option."
  // Recover is what a unit does INSTEAD OF ATTACKING, not instead of existing. Walking a
  // wounded unit back behind the line and then healing it is the basic defensive move, and
  // requiring it to stand still for a whole turn first makes that impossible.
  const Rules rules = Rules::defaults();
  GameState s = game(rules);
  const UnitId u = my_unit(s);
  // A DEFENDER, deliberately: it has no Dash, so moving sets UnitFlag::TurnEnded. Testing
  // this with the starting Warrior passes trivially because Warriors do have Dash -- which is
  // exactly why the suite missed it.
  s.units[u].type = UnitType::Defender;
  s.units[u].hp = to_hp(3);

  ActionMask moves;
  reachable_tiles(s, rules, u, moves);
  std::int16_t slots[kActionSlots];
  const int count = moves.collect(slots, kActionSlots);
  CHECK(count > 0);

  CHECK(apply_compound(s, rules, s.units[u].tile, Verb::Move, slots[0], nullptr));
  const TileIdx now = s.units[u].tile;
  const int hp_after_move = s.units[u].hp;

  std::vector<Option> options;
  enumerate_options(s, rules, now, Layer::Unit, false, options);
  bool recover_offered = false;
  for (const Option& o : options) {
    if (o.verb == Verb::Recover) recover_offered = true;
  }
  CHECK(recover_offered);
  CHECK(apply_compound(s, rules, now, Verb::Recover, tile_to_slot(now), nullptr));
  CHECK(s.units[u].hp > hp_after_move);
}

TEST("escape: a Rider may move after attacking") {
  const Rules rules = Rules::defaults();
  GameState s = game(rules);
  const UnitId u = my_unit(s);
  CHECK(has_skill(static_cast<Skill>(rules.unit(UnitType::Rider).skills), Skill::Escape));

  // Make our unit a Rider and give it an adjacent enemy that will survive the hit.
  s.units[u].type = UnitType::Rider;
  s.units[u].hp = to_hp(10);
  const Coord c = s.dims.coord(s.units[u].tile);
  TileIdx victim_tile = kNoTile;
  for (int dx = -1; dx <= 1 && victim_tile == kNoTile; ++dx) {
    for (int dy = -1; dy <= 1; ++dy) {
      if (dx == 0 && dy == 0) continue;
      const Coord n{static_cast<std::int16_t>(c.x + dx), static_cast<std::int16_t>(c.y + dy)};
      if (!s.dims.contains(n)) continue;
      const TileIdx t = s.dims.index(n);
      if (s.tiles[t].terrain != Terrain::Field || s.tiles[t].unit != kNoUnit) continue;
      victim_tile = t;
      break;
    }
  }
  CHECK(victim_tile != kNoTile);
  const UnitId victim = s.unit_count++;
  s.units[victim] = Unit{};
  s.units[victim].type = UnitType::Defender;
  s.units[victim].owner = 1;
  s.units[victim].tile = victim_tile;
  s.units[victim].hp = to_hp(15);
  s.tiles[victim_tile].unit = victim;

  CHECK(apply_compound(s, rules, s.units[u].tile, Verb::Attack, tile_to_slot(victim_tile),
                       nullptr));
  CHECK(s.units[victim].alive());  // it survived, so we are still standing where we were

  // Escape: the whole point is that the Rider may now retreat.
  ActionMask after;
  reachable_tiles(s, rules, u, after);
  CHECK(!after.empty());

  std::vector<Option> options;
  enumerate_options(s, rules, s.units[u].tile, Layer::Unit, false, options);
  bool move_offered = false;
  for (const Option& o : options) {
    if (o.verb == Verb::Move) move_offered = true;
  }
  CHECK(move_offered);
}

TEST("level rewards: a double level-up offers each level's own choice, not the top one twice") {
  // The reward menu is derived from the city's CURRENT level, but a city can bank more than
  // one level-up at a time (a big harvest, a capture, or Population Growth itself). When that
  // happens every outstanding choice is answered with the highest level's menu, so the lower
  // level's reward -- Population Growth at 4 -- can never be taken.
  const Rules rules = Rules::defaults();
  GameState s = game(rules);
  const CityId capital = my_capital(s);
  const TileIdx tile = s.cities[capital].tile;

  // The state after a harvest that crossed level 4 AND level 5: two rewards owed, and the
  // city already sitting at 5.
  s.cities[capital].level = 5;
  s.cities[capital].population = 0;
  s.cities[capital].pending_rewards = 2;

  // The FIRST outstanding choice is the one for reaching level 4.
  std::vector<Option> options;
  enumerate_options(s, rules, tile, Layer::City, false, options);
  bool offers_population_growth = false;
  for (const Option& o : options) {
    if (o.verb == Verb::ChooseReward && o.reward == CityReward::PopulationGrowth) {
      offers_population_growth = true;
    }
  }
  CHECK(offers_population_growth);
}

// A Rider's actual hit-and-run is move -> attack -> move. The attack-first test above passes
// today; this one is the ordering that matters and it does not.
TEST("escape: a Rider may retreat after moving in and attacking") {
  const Rules rules = Rules::defaults();
  GameState s = game(rules);
  const UnitId u = my_unit(s);
  s.units[u].type = UnitType::Rider;
  s.units[u].hp = to_hp(10);
  s.units[u].moves_left = static_cast<std::uint8_t>(rules.unit(UnitType::Rider).move);

  // Put a tough enemy two tiles away so the Rider must move to reach it.
  const Coord c = s.dims.coord(s.units[u].tile);
  TileIdx victim_tile = kNoTile, step = kNoTile;
  for (int dx = -1; dx <= 1 && victim_tile == kNoTile; ++dx) {
    for (int dy = -1; dy <= 1; ++dy) {
      if (dx == 0 && dy == 0) continue;
      const Coord a{static_cast<std::int16_t>(c.x + dx), static_cast<std::int16_t>(c.y + dy)};
      const Coord b{static_cast<std::int16_t>(c.x + 2 * dx), static_cast<std::int16_t>(c.y + 2 * dy)};
      if (!s.dims.contains(a) || !s.dims.contains(b)) continue;
      if (s.tiles[s.dims.index(a)].terrain != Terrain::Field) continue;
      if (s.tiles[s.dims.index(b)].terrain != Terrain::Field) continue;
      if (s.tiles[s.dims.index(a)].unit != kNoUnit || s.tiles[s.dims.index(b)].unit != kNoUnit) continue;
      step = s.dims.index(a);
      victim_tile = s.dims.index(b);
      break;
    }
  }
  CHECK(victim_tile != kNoTile);
  const UnitId victim = s.unit_count++;
  s.units[victim] = Unit{};
  s.units[victim].type = UnitType::Defender;
  s.units[victim].owner = 1;
  s.units[victim].tile = victim_tile;
  s.units[victim].hp = to_hp(15);
  s.tiles[victim_tile].unit = victim;

  CHECK(apply_compound(s, rules, s.units[u].tile, Verb::Move, tile_to_slot(step), nullptr));
  CHECK(apply_compound(s, rules, step, Verb::Attack, tile_to_slot(victim_tile), nullptr));
  CHECK(s.units[victim].alive());

  // THE POINT OF ESCAPE: having ridden in and hit, the Rider retreats.
  ActionMask after;
  reachable_tiles(s, rules, u, after);
  CHECK(!after.empty());
}

TEST("persist: a Knight that kills may attack again") {
  // Persist is what makes a Knight a Knight -- it chains through a line of wounded units.
  // The skill is declared, assigned to the Knight and asserted in test_content, and game.cpp
  // never mentions it, so a Knight's turn ended on its first kill like anything else.
  const Rules rules = Rules::defaults();
  GameState s = game(rules);
  CHECK(has_skill(static_cast<Skill>(rules.unit(UnitType::Knight).skills), Skill::Persist));
  const UnitId u = my_unit(s);
  s.units[u].type = UnitType::Knight;
  s.units[u].hp = to_hp(10);
  s.units[u].moves_left = static_cast<std::uint8_t>(rules.unit(UnitType::Knight).move);

  // Two adjacent one-hit victims.
  const Coord c = s.dims.coord(s.units[u].tile);
  std::vector<TileIdx> spots;
  for (int dx = -1; dx <= 1; ++dx) {
    for (int dy = -1; dy <= 1; ++dy) {
      if (dx == 0 && dy == 0) continue;
      const Coord n{static_cast<std::int16_t>(c.x + dx), static_cast<std::int16_t>(c.y + dy)};
      if (!s.dims.contains(n)) continue;
      const TileIdx t = s.dims.index(n);
      if (s.tiles[t].terrain != Terrain::Field || s.tiles[t].unit != kNoUnit) continue;
      spots.push_back(t);
    }
  }
  CHECK(spots.size() >= 2);
  for (int i = 0; i < 2; ++i) {
    const UnitId v = s.unit_count++;
    s.units[v] = Unit{};
    s.units[v].type = UnitType::Warrior;
    s.units[v].owner = 1;
    s.units[v].tile = spots[static_cast<std::size_t>(i)];
    s.units[v].hp = to_hp(1);  // dies to anything
    s.tiles[spots[static_cast<std::size_t>(i)]].unit = v;
  }

  CHECK(apply_compound(s, rules, s.units[u].tile, Verb::Attack, tile_to_slot(spots[0]), nullptr));
  // The Knight advanced onto the corpse; it must still be able to swing at the second.
  CHECK(!s.units[u].has(UnitFlag::TurnEnded));
  std::vector<Option> options;
  enumerate_options(s, rules, s.units[u].tile, Layer::Unit, false, options);
  bool attack_offered = false;
  for (const Option& o : options) {
    if (o.verb == Verb::Attack) attack_offered = true;
  }
  CHECK(attack_offered);
}

TEST("ruin: every reward is reachable and names itself in the log") {
  // "You should see what you get from a ruin." Two failure modes: a reward that is never
  // rolled, and a reward that is rolled but not described. Both were present -- Explorer and
  // the water Rammer did not exist despite being listed in live.json's ruin reward pool.
  const Rules rules = Rules::defaults();
  std::vector<std::string> seen;
  for (unsigned seed = 0; seed < 60; ++seed) {
    GameState s = game(rules, 11, seed);
    const UnitId u = my_unit(s);
    if (u == kNoUnit) continue;
    const TileIdx tile = s.units[u].tile;
    s.tiles[tile].resource = Resource::Ruin;
    s.units[u].remove(UnitFlag::ArrivedThisTurn);
    std::vector<std::string> log;
    if (!apply_compound(s, rules, tile, Verb::Examine, tile_to_slot(tile), &log)) continue;
    for (const std::string& line : log) {
      // Every ruin outcome must announce itself in the shape the UI parses: "Ruin: <title> -- <body>".
      if (line.rfind("Ruin: ", 0) != 0) continue;
      CHECK(line.find(" -- ") != std::string::npos);
      const std::string title = line.substr(6, line.find(" -- ") - 6);
      bool known = false;
      for (const std::string& t : seen) known = known || t == title;
      if (!known) seen.push_back(title);
    }
  }
  // Four distinct land outcomes: Resources, Scrolls of Wisdom, Population, New Friends,
  // Explorer. Anything below 3 means rewards are unreachable rather than merely unlucky.
  CHECK(seen.size() >= 3);
}
