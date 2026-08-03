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

  s.units[u].remove(UnitFlag::TurnEnded);
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
