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

// Mirrors verb_name_public in cli/main.cpp; both must track enum Verb exactly. See the note
// there — this table had the same two-entry gap, so `pending_verb` reported the wrong verb and
// indexed past the end for EndTurn and Deselect.
const char* verb_name(Verb v) {
  static const char* kNames[] = {"move",  "attack",  "capture", "harvest", "build",
                                 "train", "upgrade_naval", "research", "recover", "heal",
                                 "convert", "promote", "examine", "destroy", "disband",
                                 "choose_reward", "end_turn", "deselect"};
  static_assert(sizeof(kNames) / sizeof(kNames[0]) == kVerbCount,
                "verb name table out of sync with enum Verb");
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
  // How many of its guaranteed resource each capital needs to LEVEL UP on turn 1, if the
  // tribe's starting tech can harvest it at all. Bardur opens with Hunting and must therefore
  // find enough Game to reach level 2; Xin-xi's Metal needs Mining, so it gets one for
  // flavour and no guarantee. Computed from Rules rather than tabulated in mapgen, so the
  // thresholds and yields stay in one place.
  for (int p = 0; p < cfg.player_count; ++p) {
    const TribeStats& tribe = rules.tribe(cfg.tribes[p]);
    int count = 1;
    if (tribe.has_start_tech) {
      // Find a resource this tribe can harvest immediately, and size the guarantee to the
      // population needed for level 2.
      const int needed_population = rules.population_threshold[1];
      for (int r = 1; r < static_cast<int>(Resource::Count); ++r) {
        const ResourceStats& res = rules.resource(static_cast<Resource>(r));
        if (res.population <= 0) continue;
        if (res.needs_tech && res.tech != tribe.start_tech) continue;
        // Covers both routes to population: harvesting directly (Fruit, Game, Fish) and
        // building on the resource (Crop needs a Farm, Metal needs a Mine). Zebasi levels its
        // capital by building a Farm, not by harvesting, so restricting this to `harvestable`
        // silently excluded it.
        count = (needed_population + res.population - 1) / res.population;
        break;
      }
    }
    map.guaranteed_count[p] = count;
  }
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
    // NOT rules.unit(type).hp: promotion adds +5 to the maximum and an embarked unit keeps the
    // HP of the land unit inside the hull, so the raw type stat renders a promoted unit as
    // 15/10 and a loaded Raft against the wrong ceiling.
    w.field("max_hp", static_cast<double>(unit_max_hp(rules, unit)));
    w.field("moves_left", static_cast<int>(unit.moves_left));
    w.field("veteran", unit.has(UnitFlag::Veteran));
    w.field("turn_ended", unit.has(UnitFlag::TurnEnded) || unit.has(UnitFlag::FreshlyTrained));
    // A naval unit is a HULL CARRYING A LAND UNIT. wire-protocol.md has always documented
    // `carried`; it was simply never written, so the UI could not say what a Raft holds.
    w.key("carried");
    if (unit.has(UnitFlag::Embarked)) w.value(unit_name(unit.carried_type));
    else w.value(nullptr);
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
    // Road connection to the capital. The engine has tracked this since roads landed; it was
    // never serialised, so the UI had no way to show what a road network had actually bought.
    // Knowledge-filtered like population: whose cities are linked is your own bookkeeping.
    w.key("connected");
    if (mine) w.value(city.has(CityFlag::ConnectedToCapital) || city.has(CityFlag::Capital));
    else w.value(nullptr);
    w.field("walled", city.has(CityFlag::Walled));
    w.field("besieged", city.has(CityFlag::Besieged));
    w.field("units", static_cast<int>(city.unit_count));
    w.field("capacity", city.unit_capacity());
    // pending_rewards is purely your own bookkeeping -- an enemy's unresolved level-up is
    // not something you can see, and leaking it would telegraph exactly when they grew.
    w.field("pending_rewards", mine ? static_cast<int>(city.pending_rewards) : 0);
    // production IS visible on an enemy city. This looks like a leak and is not: the owner
    // confirmed that an enemy city's info panel shows its star production, point value and
    // capital status, even though the map decoration is a bare name plate. That is a sharper
    // distinction than "you cannot see their economy" -- the PLAYER's total stars and income
    // stay hidden; a single city you can SEE reports its own output. Tracked as U-H in
    // resolutions.md; if it turns out to be wrong, gate this on `mine`.
    w.field("production", city_income(s, rules, c));
    w.end_object();
  }
  w.end_array();

  // The v1 `legal` array is GONE. It handed the UI the agent's legality mask, which can only
  // say no -- see docs/ui-rebuild-plan.md §1. Clients use `options` instead.
  w.end_object();
  return w.text();
}

}  // namespace poly
