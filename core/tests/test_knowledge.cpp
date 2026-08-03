// The knowledge model: what a player is allowed to KNOW.
//
// This is not UI polish. The first playtest found the client receiving every player's stars,
// income and techs, because the state view was built from the engine's own omniscient state
// with only tile-level fog applied. An agent trained against that observation would be
// learning a different, easier game than the one it is meant to play.

#include <string>

#include "poly/game.hpp"
#include "poly/json.hpp"
#include "testing.hpp"

using namespace poly;

namespace {

GameState fresh_game(const Rules& rules) {
  GameConfig cfg;
  cfg.seed = 42;
  cfg.player_count = 2;
  cfg.tribes[0] = Tribe::Imperius;
  cfg.tribes[1] = Tribe::Bardur;
  cfg.is_human[0] = true;
  cfg.fog = true;
  GameState s;
  std::string error;
  new_game(cfg, rules, s, error);
  return s;
}

json::Value view_of(const GameState& s, const Rules& rules, PlayerId viewer) {
  json::Value v;
  std::string error;
  poly::json::parse(state_view_json(s, rules, viewer), v, error);
  return v;
}

}  // namespace

TEST("knowledge: starting stars are per-tribe, not a flat value") {
  // resolutions.md C2. Previously not set at all -- every tribe began on income alone.
  const Rules rules = Rules::defaults();
  CHECK_EQ(rules.tribe(Tribe::XinXi).start_stars, 7);
  CHECK_EQ(rules.tribe(Tribe::Imperius).start_stars, 5);
  CHECK_EQ(rules.tribe(Tribe::Oumaji).start_stars, 6);
  CHECK_EQ(rules.tribe(Tribe::Hoodrick).start_stars, 7);
  CHECK_EQ(rules.tribe(Tribe::Quetzali).start_stars, 7);
  CHECK_EQ(rules.tribe(Tribe::Yadakk).start_stars, 7);
  // Luxidoor pays for its level-3 capital with both stars AND its starting tech.
  CHECK_EQ(rules.tribe(Tribe::Luxidoor).start_stars, 2);
  CHECK_EQ(rules.tribe(Tribe::Luxidoor).capital_level, 3);
  CHECK(!rules.tribe(Tribe::Luxidoor).has_start_tech);
}

TEST("knowledge: a viewer never receives an opponent's stars, income or techs") {
  const Rules rules = Rules::defaults();
  GameState s = fresh_game(rules);
  const json::Value view = view_of(s, rules, 0);

  const auto& players = view["players"].arr();
  CHECK_EQ(static_cast<int>(players.size()), 2);

  // Self: everything present.
  CHECK(players[0]["stars"].is_number());
  CHECK(players[0]["income"].is_number());
  CHECK(players[0]["techs"].is_array());

  // Opponent: the three economy fields must be null, NOT merely zero. A zero would be
  // indistinguishable from a real value and would still be a leak once it stopped being zero.
  CHECK(players[1]["stars"].is_null());
  CHECK(players[1]["income"].is_null());
  CHECK(players[1]["techs"].is_null());
}

TEST("knowledge: score and city count are public even for an unmet opponent") {
  const Rules rules = Rules::defaults();
  GameState s = fresh_game(rules);
  const json::Value view = view_of(s, rules, 0);
  const auto& players = view["players"].arr();

  CHECK(players[1]["score"].is_number());
  CHECK(players[1]["cities"].is_number());
  CHECK(players[1]["score"].integer() > 0);
}

TEST("knowledge: an unmet opponent's tribe is hidden, and revealed on meeting") {
  const Rules rules = Rules::defaults();
  GameState s = fresh_game(rules);

  // Two capitals on an 11x11 start far apart and unexplored, so neither has met the other.
  CHECK(!s.players[0].has_met(1));
  {
    const json::Value view = view_of(s, rules, 0);
    CHECK(view["players"].arr()[1]["tribe"].is_null());
    CHECK(!view["players"].arr()[1]["known"].boolean());
  }

  // Force the meeting by revealing the opponent's capital.
  for (CityId c = 0; c < s.city_count; ++c) {
    if (s.cities[c].owner == 1) s.explored[0].set(s.cities[c].tile);
  }
  check_meetings(s, rules, 0, nullptr);
  CHECK(s.players[0].has_met(1));
  {
    const json::Value view = view_of(s, rules, 0);
    CHECK_EQ(view["players"].arr()[1]["tribe"].str(), std::string("bardur"));
    CHECK(view["players"].arr()[1]["known"].boolean());
    // Identity becomes known; the economy does NOT.
    CHECK(view["players"].arr()[1]["stars"].is_null());
    CHECK(view["players"].arr()[1]["income"].is_null());
  }
}

TEST("knowledge: Diplomacy reveals every tribe's identity without meeting them") {
  const Rules rules = Rules::defaults();
  GameState s = fresh_game(rules);
  CHECK(!s.players[0].has_met(1));
  s.players[0].grant_tech(Tech::Diplomacy);
  check_meetings(s, rules, 0, nullptr);
  CHECK(s.players[0].has_met(1));
  // Still not their stars.
  CHECK(view_of(s, rules, 0)["players"].arr()[1]["stars"].is_null());
}

TEST("knowledge: an enemy city's population is hidden, your own is not") {
  const Rules rules = Rules::defaults();
  GameState s = fresh_game(rules);
  // God view so both cities are in the payload regardless of fog.
  for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) s.explored[0].set(t);

  const json::Value view = view_of(s, rules, 0);
  int checked = 0;
  for (const auto& city : view["cities"].arr()) {
    if (city["owner"].integer() == 0) {
      CHECK(city["population"].is_number());
      ++checked;
    } else {
      CHECK(city["population"].is_null());
      CHECK(city["next_level_at"].is_null());
      ++checked;
    }
    // Level is visible on any city you can see.
    CHECK(city["level"].is_number());
  }
  CHECK(checked >= 2);
}

TEST("knowledge: the initial reveal is 5x5, not 3x3") {
  // resolutions.md C1. The owner's earlier "1 tile radius" was wrong; the turn-0 score
  // identity needs 25 explored tiles and the UI research confirmed 5x5 independently.
  const Rules rules = Rules::defaults();
  GameState s = fresh_game(rules);
  int explored = 0;
  for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
    if (s.explored[0].test(t)) ++explored;
  }
  // 25 for a capital with room around it; fewer only if the capital sits near an edge, and
  // map generation excludes edges from capital placement.
  CHECK_EQ(explored, 25);
}

TEST("options: unaffordable actions are reported, not deleted") {
  // The whole point of the rebuild. A legality mask can only say no; the option enumerator
  // has to say "5 stars, you have 3".
  const Rules rules = Rules::defaults();
  GameState s = fresh_game(rules);
  s.players[0].stars = 0;  // afford nothing

  CityId capital = kNoCity;
  for (CityId c = 0; c < s.city_count; ++c) {
    if (s.cities[c].owner == 0) capital = c;
  }
  CHECK(capital != kNoCity);

  // Clear the starting unit off the capital tile. Training is Blocked by TileOccupied BEFORE
  // affordability is considered -- correct ordering, since telling a player they are short of
  // stars for a unit that has nowhere to spawn would be the wrong explanation.
  const TileIdx capital_tile = s.cities[capital].tile;
  if (s.tiles[capital_tile].unit != kNoUnit) {
    s.units[s.tiles[capital_tile].unit].hp = 0;
    s.tiles[capital_tile].unit = kNoUnit;
  }

  std::vector<Option> options;
  enumerate_options(s, rules, capital_tile, Layer::City, true, options);

  int unaffordable = 0;
  for (const Option& o : options) {
    if (o.status == Availability::CannotAfford) {
      ++unaffordable;
      // The price must still be carried, or the UI cannot show what you are saving toward.
      CHECK(o.cost > 0);
    }
  }
  CHECK(unaffordable > 0);

  // And the same call with include_unavailable=false yields only actions apply() accepts.
  std::vector<Option> legal_only;
  enumerate_options(s, rules, capital_tile, Layer::City, false, legal_only);
  for (const Option& o : legal_only) CHECK(o.status == Availability::Available);
  CHECK(legal_only.size() < options.size());
}

TEST("options: a tile with a city offers no bare-tile layer") {
  // Owner correction: tapping cycles unit -> city, and there is no tile layer under a city.
  const Rules rules = Rules::defaults();
  GameState s = fresh_game(rules);
  CityId capital = kNoCity;
  for (CityId c = 0; c < s.city_count; ++c) {
    if (s.cities[c].owner == 0) capital = c;
  }
  Layer layers[4];
  const int count = tile_layers(s, s.cities[capital].tile, layers);
  CHECK(count >= 1);
  for (int i = 0; i < count; ++i) CHECK(layers[i] != Layer::Tile);
}
