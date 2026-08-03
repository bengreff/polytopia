// Game construction and the StateView serialiser.
//
// Split from game.cpp so the rules engine has no dependency on map generation or on JSON.

#include <algorithm>
#include <cmath>

#include "poly/game.hpp"
#include "poly/json.hpp"
#include "poly/mapgen.hpp"

namespace poly {
namespace {

const char* terrain_name(Terrain t) {
  switch (t) {
    case Terrain::Field: return "field";
    case Terrain::Forest: return "forest";
    case Terrain::Mountain: return "mountain";
    case Terrain::Water: return "water";
    case Terrain::Ocean: return "ocean";
    default: return "field";
  }
}

const char* resource_name(Resource r) {
  switch (r) {
    case Resource::Fruit: return "fruit";
    case Resource::Crop: return "crop";
    case Resource::Game: return "game";
    case Resource::Fish: return "fish";
    case Resource::Starfish: return "starfish";
    case Resource::Metal: return "metal";
    case Resource::Ruin: return "ruin";
    case Resource::Village: return "village";
    case Resource::Lighthouse: return "lighthouse";
    default: return "none";
  }
}

const char* improvement_name(Improvement i) {
  switch (i) {
    case Improvement::LumberHut: return "lumber_hut";
    case Improvement::Sawmill: return "sawmill";
    case Improvement::Farm: return "farm";
    case Improvement::Windmill: return "windmill";
    case Improvement::Mine: return "mine";
    case Improvement::Forge: return "forge";
    case Improvement::Port: return "port";
    case Improvement::Market: return "market";
    case Improvement::Temple: return "temple";
    case Improvement::ForestTemple: return "forest_temple";
    case Improvement::MountainTemple: return "mountain_temple";
    case Improvement::WaterTemple: return "water_temple";
    default: return "none";
  }
}

const char* unit_name(UnitType t) {
  static const char* kNames[] = {"warrior", "rider",   "archer", "defender", "swordsman",
                                 "mind_bender", "catapult", "knight", "cloak", "dagger",
                                 "giant",   "explorer", "raft",  "scout",  "rammer",
                                 "bomber",  "juggernaut", "dinghy", "pirate"};
  const int index = static_cast<int>(t);
  return (index >= 0 && index < kUnitTypeCount) ? kNames[index] : "warrior";
}

const char* tribe_name(Tribe t) {
  static const char* kNames[] = {"xin_xi", "imperius", "bardur", "oumaji",
                                 "kickoo", "hoodrick", "luxidoor", "vengir",
                                 "zebasi", "ai_mo",    "quetzali", "yadakk"};
  const int index = static_cast<int>(t);
  return (index >= 0 && index < kTribeCount) ? kNames[index] : "xin_xi";
}

const char* tech_name(Tech t) {
  static const char* kNames[] = {
      "climbing",   "mining",     "smithery",   "meditation", "philosophy",
      "fishing",    "sailing",    "navigation", "ramming",    "aquatism",
      "hunting",    "archery",    "spiritualism", "forestry", "mathematics",
      "organization", "farming",  "construction", "strategy", "diplomacy",
      "riding",     "roads",      "trade",      "free_spirit", "chivalry"};
  const int index = static_cast<int>(t);
  return (index >= 0 && index < kTechCount) ? kNames[index] : "climbing";
}

const char* verb_name(Verb v) {
  static const char* kNames[] = {"move",  "attack",  "capture", "harvest", "build",
                                 "train", "upgrade_naval", "research", "heal", "convert",
                                 "examine", "destroy", "disband", "choose_reward",
                                 "end_turn", "deselect"};
  const int index = static_cast<int>(v);
  return (index >= 0 && index < kVerbCount) ? kNames[index] : "move";
}

const char* phase_name(Phase p) {
  switch (p) {
    case Phase::Select: return "select";
    case Phase::Verb: return "verb";
    case Phase::Target: return "target";
    default: return "select";
  }
}

const char* reward_name(CityReward r) {
  switch (r) {
    case CityReward::Workshop: return "Workshop (+1 star/turn)";
    case CityReward::Explorer: return "Explorer";
    case CityReward::CityWall: return "City Wall";
    case CityReward::Resources: return "Resources (+5 stars)";
    case CityReward::PopulationGrowth: return "Population Growth (+3)";
    case CityReward::BorderGrowth: return "Border Growth";
    case CityReward::Park: return "Park (+250 score)";
    case CityReward::SuperUnit: return "Super Unit (Giant)";
    default: return "reward";
  }
}

// A human-readable label for one legal action, so the UI never has to decode slot indices
// itself. Keeping this server-side is what lets the UI honour "never construct an action
// index by arithmetic" -- it only ever echoes back an index it was handed.
void describe_action(const GameState& s, const Rules& rules, int index, json::Writer& w) {
  w.begin_object();
  w.field("index", index);

  // Checked FIRST: the global actor must never fall through to a typed-slot test.
  if (s.phase == Phase::Select && index == kGlobalActorSlot) {
    w.field("kind", "global");
    w.field("label", "Player actions");
    w.end_object();
    return;
  }

  if (s.phase == Phase::Verb) {
    w.field("kind", "verb");
    w.field("label", verb_name(static_cast<Verb>(index)));
    w.end_object();
    return;
  }
  if (slot_is_train(index)) {
    const UnitType type = slot_to_unit_type(index);
    w.field("kind", "train");
    w.field("label", std::string(unit_name(type)) + " (" +
                         std::to_string(rules.unit(type).cost) + "*)");
    w.end_object();
    return;
  }
  if (slot_is_research(index)) {
    const Tech tech = slot_to_tech(index);
    int cities = 0;
    for (CityId c = 0; c < s.city_count; ++c) {
      if (s.cities[c].owner == s.current_player) ++cities;
    }
    const int cost = rules.tech_cost(tech, cities, s.me().has_tech(Tech::Philosophy));
    w.field("kind", "research");
    w.field("label", std::string(tech_name(tech)) + " (" + std::to_string(cost) + "*)");
    w.end_object();
    return;
  }
  if (slot_is_build(index)) {
    const Improvement imp = slot_to_improvement(index);
    w.field("kind", "build");
    w.field("label", std::string(improvement_name(imp)) + " (" +
                         std::to_string(rules.improvement(imp).cost) + "*)");
    w.end_object();
    return;
  }
  if (slot_is_reward(index)) {
    w.field("kind", "reward");
    w.field("label", reward_name(slot_to_reward(index)));
    w.end_object();
    return;
  }
  w.field("kind", "tile");
  w.field("label", "");
  w.end_object();
}

}  // namespace

bool new_game(const GameConfig& cfg, const Rules& rules, GameState& out, std::string& error) {
  if (cfg.player_count < 2 || cfg.player_count > kMaxPlayers) {
    error = "player_count must be between 2 and " + std::to_string(kMaxPlayers);
    return false;
  }
  // Opponents are bounded by settlement sites, not by the array size.
  const int seats = max_players_for_map(cfg.dims, cfg.land_ratio, rules.village_density);
  if (cfg.player_count > seats) {
    error = "this map seats at most " + std::to_string(seats) + " players";
    return false;
  }

  MapConfig map;
  map.dims = cfg.dims;
  map.seed = cfg.seed;
  map.land_ratio = cfg.land_ratio;
  map.village_density = rules.village_density;
  map.ruin_density = rules.ruin_density;
  map.player_count = cfg.player_count;
  for (int p = 0; p < cfg.player_count; ++p) map.tribes[p] = cfg.tribes[p];

  // Generation runs FIRST and owns state initialisation -- it resets GameState wholesale.
  // Anything configured before this call would be silently discarded, which is exactly the
  // bug that made turn limits never fire: games ran to turn 521 against a limit of 60.
  if (!generate_map(map, out, error)) return false;

  out.mode = cfg.mode;
  out.turn_limit = cfg.turn_limit;
  out.fog = cfg.fog;
  out.player_count = static_cast<std::int8_t>(cfg.player_count);
  out.turn = 1;
  out.current_player = 0;

  for (int p = 0; p < cfg.player_count; ++p) {
    Player& player = out.players[p];
    const TribeStats& tribe = rules.tribe(cfg.tribes[p]);
    player.tribe = cfg.tribes[p];
    player.is_human = cfg.is_human[p];
    player.difficulty = cfg.difficulty[p];
    player.alive = true;
    // Starting stars vary BY TRIBE and were previously not set at all, so every tribe began
    // on income alone (resolutions.md C2). Luxidoor's 2 buys its level-3 capital.
    player.stars = tribe.start_stars;
    // A tribe's starting tech is GRANTED FREE, not discounted (live-rules.md §3.3).
    // Luxidoor has none.
    if (tribe.has_start_tech) player.grant_tech(tribe.start_tech);
    player.mark_met(static_cast<PlayerId>(p));  // you always know yourself
  }

  for (PlayerId p = 0; p < out.player_count; ++p) recompute_visibility(out, rules, p);

  // The opening player's turn has to be STARTED, not just entered. Income is credited at
  // turn start (resolutions.md R1), and nothing has changed hands yet, so without this the
  // first player begins the game on zero stars and cannot act at all on turn 1.
  begin_turn(out, rules);

  out.phase = Phase::Select;
  out.selected = kNoTile;
  return true;
}

std::string state_view_json(const GameState& s, const Rules& rules, PlayerId viewer) {
  json::Writer w;
  const bool omniscient = viewer == kNoPlayer || !s.fog;

  w.begin_object();
  w.key("dims");
  w.begin_object();
  w.field("width", static_cast<int>(s.dims.width));
  w.field("height", static_cast<int>(s.dims.height));
  w.end_object();

  w.field("turn", s.turn);
  w.field("current_player", static_cast<int>(s.current_player));
  w.field("phase", phase_name(s.phase));
  w.field("selected", static_cast<int>(s.selected));
  w.field("pending_verb", verb_name(s.pending_verb));
  // A level-up must be resolved IMMEDIATELY; the engine refuses every other verb until it is,
  // so the UI has to raise a modal rather than leave the player wondering why nothing works.
  w.field("pending_reward_city", static_cast<int>(pending_reward_city(s)));
  // End Turn lights up only when NOTHING at all remains -- not merely when units are spent.
  w.field("has_actions", has_any_available_action(s, rules));

  const MaybeOutcome result = outcome(s, rules);
  w.key("outcome");
  if (!result.has_value()) {
    // A horizon cutoff is nullopt exactly like any other unfinished position -- see
    // outcome.hpp. There is no representation in which the two could be confused.
    w.value(nullptr);
  } else {
    w.begin_object();
    w.field("how", result->how == Termination::LastStanding ? "domination" : "score");
    w.field("winner", static_cast<int>(result->winner));
    w.field("turn", result->turn);
    w.end_object();
  }

  w.key("players");
  w.begin_array();
  for (PlayerId p = 0; p < s.player_count; ++p) {
    const Player& player = s.players[p];
    // KNOWLEDGE FILTER. Score and city count are permanently public; stars, income, techs
    // and tribe identity are not (docs/ui-rebuild-plan.md §3). Enforced HERE, server-side,
    // for the same reason fog is: a UI bug must not be able to leak it, and an agent trained
    // against a leaky observation would be learning a different game.
    const bool self = (p == viewer);
    const bool known = omniscient || self || s.players[viewer].has_met(p);

    int cities = 0;
    for (CityId c = 0; c < s.city_count; ++c) {
      if (s.cities[c].owner == p) ++cities;
    }

    w.begin_object();
    w.field("id", static_cast<int>(p));
    w.field("known", known);
    w.field("human", player.is_human);
    w.field("alive", player.alive);
    // Always public.
    w.field("score", player_score(s, rules, p));
    w.field("cities", cities);
    // Identity: only once met, or with Diplomacy.
    if (known) {
      w.field("tribe", tribe_name(player.tribe));
    } else {
      w.key("tribe");
      w.value(nullptr);
    }
    // NEVER visible for anyone but yourself -- not even a met opponent.
    if (self || omniscient) {
      w.field("stars", player.stars);
      w.field("income", player_income(s, rules, p));
      w.key("techs");
      w.begin_array();
      for (int i = 0; i < kTechCount; ++i) {
        if (player.has_tech(static_cast<Tech>(i))) w.value(tech_name(static_cast<Tech>(i)));
      }
      w.end_array();
    } else {
      w.key("stars");
      w.value(nullptr);
      w.key("income");
      w.value(nullptr);
      w.key("techs");
      w.value(nullptr);
    }
    w.end_object();
  }
  w.end_array();

  // Fog is applied HERE, server-side. Unexplored tiles are omitted from the payload
  // entirely, so no UI bug can leak the map.
  const auto visible = [&](TileIdx t) { return omniscient || s.explored[viewer].test(t); };

  w.key("tiles");
  w.begin_array();
  for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
    if (!visible(t)) continue;
    const Tile& tile = s.tiles[t];
    w.begin_object();
    w.field("i", static_cast<int>(t));
    w.field("terrain", terrain_name(tile.terrain));
    w.field("resource", resource_name(tile.resource));
    w.field("improvement", improvement_name(tile.improvement));
    w.field("owner", static_cast<int>(tile.owner));
    w.field("city", static_cast<int>(tile.city));
    w.field("road", tile.has(TileFlag::Road) || tile.has(TileFlag::Bridge));
    w.end_object();
  }
  w.end_array();

  w.key("units");
  w.begin_array();
  for (UnitId u = 0; u < s.unit_count; ++u) {
    const Unit& unit = s.units[u];
    if (!unit.alive() || unit.tile == kNoTile) continue;
    if (!visible(unit.tile)) continue;
    w.begin_object();
    w.field("id", static_cast<int>(u));
    w.field("type", unit_name(unit.type));
    w.field("owner", static_cast<int>(unit.owner));
    w.field("tile", static_cast<int>(unit.tile));
    // HP is stored in half points and can legitimately land on .5 (resolutions.md R13).
    w.field("hp", static_cast<double>(unit.hp) / kHpScale);
    w.field("max_hp", static_cast<double>(rules.unit(unit.type).hp));
    w.field("moves_left", static_cast<int>(unit.moves_left));
    w.field("veteran", unit.has(UnitFlag::Veteran));
    w.field("turn_ended", unit.has(UnitFlag::TurnEnded) || unit.has(UnitFlag::FreshlyTrained));
    w.end_object();
  }
  w.end_array();

  w.key("cities");
  w.begin_array();
  for (CityId c = 0; c < s.city_count; ++c) {
    const City& city = s.cities[c];
    if (!visible(city.tile)) continue;
    w.begin_object();
    w.field("id", static_cast<int>(c));
    w.field("owner", static_cast<int>(city.owner));
    w.field("tile", static_cast<int>(city.tile));
    w.field("level", static_cast<int>(city.level));
    // Population is visible ONLY on your own cities (ui-reference.md §5). May be NEGATIVE --
    // render as such, never clamp (resolutions.md R17).
    const bool mine = omniscient || city.owner == viewer;
    if (mine) {
      w.field("population", static_cast<int>(city.population));
      w.field("next_level_at", population_needed(s, rules, c));
    } else {
      w.key("population");
      w.value(nullptr);
      w.key("next_level_at");
      w.value(nullptr);
    }
    w.field("capital", city.has(CityFlag::Capital));
    w.field("walled", city.has(CityFlag::Walled));
    w.field("besieged", city.has(CityFlag::Besieged));
    w.field("units", static_cast<int>(city.unit_count));
    w.field("capacity", city.unit_capacity());
    w.field("pending_rewards", static_cast<int>(city.pending_rewards));
    w.field("production", city_income(s, rules, c));
    w.end_object();
  }
  w.end_array();

  w.key("legal");
  w.begin_array();
  ActionMask mask;
  legal_actions(s, rules, mask);
  std::int16_t slots[kActionSlots];
  const int count = mask.collect(slots, kActionSlots);
  for (int i = 0; i < count; ++i) describe_action(s, rules, slots[i], w);
  w.end_array();

  w.end_object();
  return w.text();
}

}  // namespace poly
