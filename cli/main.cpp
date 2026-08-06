// poly-cli -- the native entry point.
//
//   --rpc         newline-delimited JSON-RPC on stdin/stdout (see docs/wire-protocol.md)
//   --selfplay N  play N random-legal games; the whole-engine smoke test
//   --bench       forward-model throughput
//
// The RPC mode is what web/server.js drives. It is deliberately dumb: one request in, one
// response out, no state on the Node side. When the WASM target lands this whole path is
// deleted and the renderer talks to the module directly.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "poly/game.hpp"
#include "poly/mapgen.hpp"
#include "poly/json.hpp"
#include "poly/version.hpp"

#include <memory>

#include "agents/agent.hpp"
#include "agents/ladder.hpp"
#include "agents/registry.hpp"
#include "wire/state_view.hpp"

namespace poly::audit {
int run(int games, const Rules& rules, bool verbose);
int run_seed(std::uint64_t seed, const Rules& rules);
}  // namespace poly::audit

namespace {

using namespace poly;

Rules g_rules = Rules::defaults();
GameState g_state;
std::string g_rules_path = "data/rules/live.json";
bool g_have_game = false;

Tribe parse_tribe(const std::string& name, Tribe fallback) {
  static const struct {
    const char* name;
    Tribe tribe;
  } kTable[] = {{"xin_xi", Tribe::XinXi},     {"imperius", Tribe::Imperius},
                {"bardur", Tribe::Bardur},    {"oumaji", Tribe::Oumaji},
                {"kickoo", Tribe::Kickoo},    {"hoodrick", Tribe::Hoodrick},
                {"luxidoor", Tribe::Luxidoor}, {"vengir", Tribe::Vengir},
                {"zebasi", Tribe::Zebasi},    {"ai_mo", Tribe::AiMo},
                {"quetzali", Tribe::Quetzali}, {"yadakk", Tribe::Yadakk}};
  for (const auto& entry : kTable) {
    if (name == entry.name) return entry.tribe;
  }
  return fallback;
}

Difficulty parse_difficulty(const std::string& name) {
  if (name == "easy") return Difficulty::Easy;
  if (name == "hard") return Difficulty::Hard;
  if (name == "crazy") return Difficulty::Crazy;
  return Difficulty::Normal;
}

GameMode parse_mode(const std::string& name) {
  if (name == "perfection") return GameMode::Perfection;
  if (name == "glory") return GameMode::Glory;
  if (name == "creative") return GameMode::Creative;
  return GameMode::Domination;
}

// MUST stay in lockstep with enum Verb (action.hpp). It silently fell two entries behind when
// Recover and Promote landed, which did not fail loudly: every verb after Research shifted
// down one, so `options` advertised Promote as "destroy" and Disband as "end_turn", and the
// last two indices read off the end of the array. The static_assert makes the next omission a
// compile error instead of a wrong verb on the wire.
const char* verb_name_public(Verb v) {
  static const char* kNames[] = {"move",  "attack",  "capture", "harvest", "build",
                                 "train", "upgrade_naval", "research", "recover", "heal",
                                 "convert", "promote", "examine", "destroy", "disband",
                                 "choose_reward", "end_turn", "deselect"};
  static_assert(sizeof(kNames) / sizeof(kNames[0]) == kVerbCount,
                "verb name table out of sync with enum Verb");
  const int i = static_cast<int>(v);
  return (i >= 0 && i < kVerbCount) ? kNames[i] : "move";
}

const char* unit_name_public(UnitType t) {
  static const char* kNames[] = {"Warrior", "Rider", "Archer", "Defender", "Swordsman",
                                 "Mind Bender", "Catapult", "Knight", "Cloak", "Dagger",
                                 "Giant", "Explorer", "Raft", "Scout", "Rammer", "Bomber",
                                 "Juggernaut", "Dinghy", "Pirate"};
  const int i = static_cast<int>(t);
  return (i >= 0 && i < kUnitTypeCount) ? kNames[i] : "Warrior";
}

// Road and Bridge are tile FLAGS rather than buildings, but they still occupy Improvement
// slots so that `build` can target them — and this table stopped at Water Temple, so both new
// entries indexed past the end and shipped a garbage label on a real, buildable option.
const char* improvement_name_public(Improvement imp) {
  static const char* kNames[] = {"None", "Lumber Hut", "Sawmill", "Farm", "Windmill", "Mine",
                                 "Forge", "Port", "Market", "Temple", "Forest Temple",
                                 "Mountain Temple", "Water Temple", "Road", "Bridge"};
  static_assert(sizeof(kNames) / sizeof(kNames[0]) == static_cast<int>(Improvement::Count),
                "improvement name table out of sync with enum Improvement");
  const int i = static_cast<int>(imp);
  return (i >= 0 && i < static_cast<int>(Improvement::Count)) ? kNames[i] : "None";
}

const char* tech_name_public(Tech t) {
  static const char* kNames[] = {
      "Climbing", "Mining", "Smithery", "Meditation", "Philosophy", "Fishing", "Sailing",
      "Navigation", "Ramming", "Aquatism", "Hunting", "Archery", "Spiritualism", "Forestry",
      "Mathematics", "Organization", "Farming", "Construction", "Strategy", "Diplomacy",
      "Riding", "Roads", "Trade", "Free Spirit", "Chivalry"};
  const int i = static_cast<int>(t);
  return (i >= 0 && i < kTechCount) ? kNames[i] : "Climbing";
}

const char* reward_name_public(CityReward r) {
  switch (r) {
    case CityReward::Workshop: return "Workshop";
    case CityReward::Explorer: return "Explorer";
    case CityReward::CityWall: return "City Wall";
    case CityReward::Resources: return "Resources";
    case CityReward::PopulationGrowth: return "Population Growth";
    case CityReward::BorderGrowth: return "Border Growth";
    case CityReward::Park: return "Park";
    default: return "Super Unit";
  }
}

// The display name for one option. Built server-side so the UI never has to decode a slot
// index -- the same reason labels ship with legal actions.
std::string option_label(const Rules&, const Option& o) {
  switch (o.verb) {
    case Verb::Train: return unit_name_public(o.unit_type);
    case Verb::Build: return improvement_name_public(o.improvement);
    case Verb::Research: return tech_name_public(o.tech);
    case Verb::ChooseReward: return reward_name_public(o.reward);
    // Raft -> Scout / Rammer / Bomber: the hull it becomes IS the option.
    case Verb::UpgradeNaval: return unit_name_public(o.unit_type);
    case Verb::Move: return "Move";
    case Verb::Attack: return "Attack";
    case Verb::Capture: return "Capture";
    case Verb::Harvest: return "Harvest";
    case Verb::Examine: return "Examine ruin";
    case Verb::Recover: return "Recover";
    case Verb::Promote: return "Promote to Veteran";
    case Verb::Heal: return "Heal Others";
    case Verb::Convert: return "Convert";
    case Verb::Destroy: return "Destroy Building";
    case Verb::Disband: return "Disband";
    // Falling through to the wire name would print a lowercase enum ("promote", "disband")
    // straight into a button, which is how the new verbs first appeared.
    default: return verb_name_public(o.verb);
  }
}

const char* availability_name(Availability a) {
  switch (a) {
    case Availability::Available: return "available";
    case Availability::CannotAfford: return "cannot_afford";
    case Availability::MissingTech: return "missing_tech";
    default: return "blocked";
  }
}

const char* reason_name(BlockReason r) {
  switch (r) {
    case BlockReason::CityFull: return "too many";
    case BlockReason::TileOccupied: return "tile occupied";
    case BlockReason::WrongTerrain: return "wrong terrain";
    case BlockReason::NoResource: return "no resource";
    case BlockReason::AlreadyBuilt: return "already built";
    case BlockReason::PerCityLimit: return "one per city";
    case BlockReason::NotYourTerritory: return "not your territory";
    case BlockReason::UnitSpent: return "already acted";
    case BlockReason::FreshlyTrained: return "trained this turn";
    case BlockReason::NeedsTurnStart: return "must start the turn here";
    case BlockReason::OutOfRange: return "nothing in range";
    case BlockReason::PendingReward: return "resolve the level-up first";
    default: return "";
  }
}

// The inverse of verb_name_public, and it must cover EVERY verb that enumerate_options can
// hand out. It covered eleven of eighteen, and the fallthrough is Deselect — so `act` on a
// recover/promote/heal/convert/disband/destroy/upgrade_naval option quietly deselected the
// unit and returned a success-shaped StateView in which nothing had happened. Parsed by the
// same table that prints, so the two cannot drift again.
Verb parse_verb(const std::string& name) {
  for (int i = 0; i < kVerbCount; ++i) {
    const Verb v = static_cast<Verb>(i);
    if (name == verb_name_public(v)) return v;
  }
  return Verb::Deselect;
}

Layer parse_layer(const std::string& name) {
  if (name == "unit") return Layer::Unit;
  if (name == "city") return Layer::City;
  return Layer::Tile;
}

std::string error_response(int id, const std::string& message) {
  json::Writer w;
  w.begin_object();
  w.field("id", id);
  w.field("error", message);
  w.end_object();
  return w.text();
}

// Runs every non-human player until it is a human's decision again, or the game ends.
//
// THIS IS THE OPPONENT A HUMAN ACTUALLY PLAYS. It used to pick uniformly at random from the
// legality mask -- the ladder's floor agent, rated ~1200 Elo below `rulebased` -- so none of
// the agent work reached the browser at all, and the game was unplayable as a test of
// anything. The comment here promised "bots/scripted.cpp replaces this"; that file was never
// written. `poly_agents` was already linked into this binary, so the fix is to call the
// agent the ladder measures rather than to write a third policy that could drift from it.
EventLog advance_bots(GameState& s, const Rules& rules, int step_budget) {
  EventLog log;

  // Held across calls so the agent's RNG advances over the whole game instead of replaying
  // the same stream every time the human ends a turn.
  static std::unique_ptr<agents::Agent> bot = agents::make_agent("rulebased");
  static bool seeded = false;
  if (!seeded) {
    bot->seed(0x9E3779B97F4A7C15ull);
    seeded = true;
  }
  if (!bot) return log;

  // The same backstop the ladder carries (ladder.cpp), for the same reason: the phase machine
  // permits no-op cycles, so a turn that fails to end must be ended for it. `rulebased` no
  // longer needs this -- it ends its own turns -- but the RPC server must not be able to hang
  // the browser on a future agent that does, since here there is no match harness to notice.
  // MEASURED, not guessed. `--audit` reports the largest honest player-turn it sees; on the
  // default 16x16 four-tribe game late on, that is ~400 micro-decisions, because every unit,
  // city and buildable tile costs three. The old value of 300 therefore sat BELOW real play
  // and force-ended the bot mid-turn -- silently discarding the tail of its work, which is a
  // strength bug rather than the safety net it was meant to be. A backstop only has to be
  // clear of honest play: a genuine no-op cycle is unbounded.
  const int kMaxDecisionsPerTurn = 20000;
  int decisions_this_turn = 0;
  std::int32_t turn_at_last = s.turn;
  PlayerId mover_at_last = s.current_player;

  int steps = 0;
  while (steps++ < step_budget && !outcome(s, rules).has_value()) {
    if (s.players[s.current_player].is_human) break;
    ActionMask mask;
    legal_actions(s, rules, mask);
    if (mask.empty()) break;

    if (s.turn != turn_at_last || s.current_player != mover_at_last) {
      decisions_this_turn = 0;
      turn_at_last = s.turn;
      mover_at_last = s.current_player;
    }
    if (++decisions_this_turn > kMaxDecisionsPerTurn) {
      if (s.phase != Phase::Select) {
        apply(s, rules, Action{static_cast<std::int16_t>(Verb::Deselect)}, &log);
      }
      apply(s, rules, Action{static_cast<std::int16_t>(kGlobalActorSlot)}, &log);
      apply(s, rules, Action{static_cast<std::int16_t>(Verb::EndTurn)}, &log);
      continue;
    }

    // A fresh budget per decision, matching the ladder so the browser opponent and the
    // measured one are the same agent under the same conditions.
    agents::ForwardModel model(rules, 2000);
    Action action = bot->choose(s, model, mask, s.current_player);
    if (!mask.test(action.index)) {
      const int count = mask.count();
      std::int16_t slots[kActionSlots];
      mask.collect(slots, kActionSlots);
      action = Action{slots[s.rng.bounded(static_cast<std::uint32_t>(count))]};
    }
    if (!apply(s, rules, action, &log)) break;
  }
  return log;
}

std::string handle_request(const json::Value& request) {
  const int id = request["id"].integer();
  const std::string method = request["method"].str();
  const json::Value& params = request["params"];

  // The engine's default New Game setup, so the UI does not have to carry its own copy.
  //
  // It did carry one, and it was wrong: the browser opened an 11x11 duel while GameConfig said
  // 16x16 with four tribes. That made SIX places holding this value -- GameConfig, MatchConfig,
  // MapConfig, app.js's DEFAULT_CONFIG and index.html's two form defaults -- of which four
  // disagreed with the engine. Three were fixed by pointing them at one constant; the two in
  // the browser cannot see that constant, so they ask for it instead.
  if (method == "defaults") {
    const GameConfig cfg;
    json::Writer w;
    w.begin_object();
    w.field("id", id);
    w.key("result");
    w.begin_object();
    w.field("width", cfg.dims.width);
    w.field("height", cfg.dims.height);
    w.field("players", cfg.player_count);
    w.field("fog", cfg.fog);
    w.field("land_ratio", cfg.land_ratio);
    w.end_object();
    w.end_object();
    return w.text();
  }

  if (method == "map_info") {
    // How many players a map of this size can seat, so the UI can bound its opponent picker
    // instead of offering counts that generation will then refuse.
    MapDims dims{static_cast<std::int16_t>(params["width"].integer(11)),
                 static_cast<std::int16_t>(params["height"].integer(11))};
    const auto land = static_cast<float>(params["land_ratio"].num(0.6));
    json::Writer w;
    w.begin_object();
    w.field("id", id);
    w.key("result");
    w.begin_object();
    w.field("max_players", max_players_for_map(dims, land, g_rules.village_density));
    w.field("expected_villages",
            static_cast<int>(static_cast<float>(dims.tile_count()) * land *
                                 g_rules.village_density + 0.5f));
    w.end_object();
    w.end_object();
    return w.text();
  }

  if (method == "new_game") {
    GameConfig cfg;
    cfg.dims.width = static_cast<std::int16_t>(params["width"].integer(16));
    cfg.dims.height = static_cast<std::int16_t>(params["height"].integer(16));
    cfg.seed = static_cast<std::uint64_t>(params["seed"].integer(1));
    cfg.mode = parse_mode(params["mode"].str());
    // No turn limit by default -- games end by domination. Perfection still sets one.
    cfg.turn_limit = params["turn_limit"].integer(0);
    cfg.fog = params["fog"].boolean(true);
    cfg.land_ratio = static_cast<float>(params["land_ratio"].num(0.6));

    const auto& players = params["players"].arr();
    cfg.player_count = static_cast<int>(players.size());
    if (cfg.player_count < 2) {
      // Base-game default: four tribes, yours chosen and the rest random.
      cfg.player_count = 4;
      cfg.tribes[0] = Tribe::Imperius;
      cfg.is_human[0] = true;
      Pcg32 pick;
      pick.seed(cfg.seed, 0xA11CE);
      bool used[kTribeCount] = {};
      used[static_cast<int>(cfg.tribes[0])] = true;
      for (int i = 1; i < cfg.player_count; ++i) {
        int choice = 0;
        for (int attempt = 0; attempt < 64; ++attempt) {
          choice = static_cast<int>(pick.bounded(kTribeCount));
          if (!used[choice]) break;
        }
        used[choice] = true;
        cfg.tribes[i] = static_cast<Tribe>(choice);
      }
    } else {
      // "random" picks an unused tribe. The UI randomises opponents; training pins them
      // explicitly by name, so the same entry point serves both without a second code path.
      bool taken[kTribeCount] = {};
      std::vector<int> random_seats;
      for (int i = 0; i < cfg.player_count && i < kMaxPlayers; ++i) {
        const std::string name = players[static_cast<std::size_t>(i)]["tribe"].str();
        cfg.is_human[i] = players[static_cast<std::size_t>(i)]["human"].boolean(i == 0);
        cfg.difficulty[i] =
            parse_difficulty(players[static_cast<std::size_t>(i)]["difficulty"].str());
        if (name.empty() || name == "random") {
          random_seats.push_back(i);
          continue;
        }
        cfg.tribes[i] = parse_tribe(name, static_cast<Tribe>(i % kTribeCount));
        taken[static_cast<int>(cfg.tribes[i])] = true;
      }
      // Drawn from a seeded generator so a given seed reproduces the whole matchup.
      Pcg32 picker;
      picker.seed(cfg.seed, 0xA11CE);
      for (const int seat : random_seats) {
        int choice = 0;
        for (int attempt = 0; attempt < 64; ++attempt) {
          choice = static_cast<int>(picker.bounded(kTribeCount));
          if (!taken[choice]) break;
        }
        if (taken[choice]) {
          for (int t = 0; t < kTribeCount; ++t) {
            if (!taken[t]) {
              choice = t;
              break;
            }
          }
        }
        taken[choice] = true;
        cfg.tribes[seat] = static_cast<Tribe>(choice);
      }
    }

    std::string error;
    if (!new_game(cfg, g_rules, g_state, error)) return error_response(id, error);
    g_have_game = true;
  } else if (!g_have_game) {
    return error_response(id, "no game in progress; call new_game first");
  }

  if (method == "apply") {
    const int index = params["index"].integer(-1);
    if (!apply(g_state, g_rules, Action{static_cast<std::int16_t>(index)}, nullptr)) {
      return error_response(id, "illegal action " + std::to_string(index));
    }
  }

  if (method == "reload_rules") {
    Rules loaded;
    std::string error;
    if (!load_rules(g_rules_path, loaded, error)) return error_response(id, error);
    g_rules = loaded;
    json::Writer w;
    w.begin_object();
    w.field("id", id);
    w.key("result");
    w.begin_object();
    w.field("ok", true);
    w.field("version", g_rules.version);
    w.field("unverified_count", static_cast<int>(g_rules.unverified.size()));
    w.end_object();
    w.end_object();
    return w.text();
  }

  if (method == "options" || method == "research_options") {
    std::vector<Option> options;
    const bool include = params["include_unavailable"].boolean(true);
    if (method == "research_options") {
      enumerate_research(g_state, g_rules, include, options);
    } else {
      enumerate_options(g_state, g_rules,
                        static_cast<TileIdx>(params["tile"].integer(-1)),
                        parse_layer(params["layer"].str()), include, options);
    }
    json::Writer w;
    w.begin_object();
    w.field("id", id);
    w.key("result");
    w.begin_object();
    w.key("options");
    w.begin_array();
    for (const Option& o : options) {
      w.begin_object();
      w.field("verb", verb_name_public(o.verb));
      w.field("status", availability_name(o.status));
      w.field("cost", o.cost);
      w.field("target", static_cast<int>(o.action.index));
      w.field("label", option_label(g_rules, o));
      if (o.status == Availability::MissingTech) {
        w.field("needs_tech", tech_name_public(o.required_tech));
      }
      if (o.status == Availability::Blocked) w.field("reason", reason_name(o.reason));
      w.end_object();
    }
    w.end_array();
    w.end_object();
    w.end_object();
    return w.text();
  }

  if (method == "layers") {
    Layer layers[4];
    const int count = tile_layers(g_state, static_cast<TileIdx>(params["tile"].integer(-1)), layers);
    json::Writer w;
    w.begin_object();
    w.field("id", id);
    w.key("result");
    w.begin_object();
    w.key("layers");
    w.begin_array();
    for (int i = 0; i < count; ++i) {
      w.value(layers[i] == Layer::Unit ? "unit" : (layers[i] == Layer::City ? "city" : "tile"));
    }
    w.end_array();
    w.end_object();
    w.end_object();
    return w.text();
  }

  if (method == "preview_combat") {
    const CombatPreview preview =
        preview_combat(g_state, g_rules, static_cast<TileIdx>(params["from"].integer(-1)),
                       static_cast<TileIdx>(params["to"].integer(-1)));
    json::Writer w;
    w.begin_object();
    w.field("id", id);
    w.key("result");
    w.begin_object();
    w.field("valid", preview.valid);
    w.field("attacker_damage", preview.attacker_damage);
    w.field("defender_damage", preview.defender_damage);
    w.field("defender_dies", preview.defender_dies);
    w.field("attacker_dies", preview.attacker_dies);
    w.end_object();
    w.end_object();
    return w.text();
  }

  if (method == "reachable") {
    const auto tile = static_cast<TileIdx>(params["tile"].integer(-1));
    ActionMask mask;
    const UnitId unit = (tile >= 0 && tile < static_cast<TileIdx>(g_state.tile_count()))
                            ? g_state.tiles[tile].unit
                            : kNoUnit;
    if (unit != kNoUnit) reachable_tiles(g_state, g_rules, unit, mask);
    json::Writer w;
    w.begin_object();
    w.field("id", id);
    w.key("result");
    w.begin_object();
    w.key("tiles");
    w.begin_array();
    for (TileIdx t = 0; t < static_cast<TileIdx>(g_state.tile_count()); ++t) {
      if (mask.test(tile_to_slot(t))) w.value(static_cast<int>(t));
    }
    w.end_array();
    w.end_object();
    w.end_object();
    return w.text();
  }

  EventLog log;
  if (method == "act") {
    // One human-scale action, driven through the phase machine by apply_compound.
    const auto tile = static_cast<TileIdx>(params["tile"].integer(-1));
    const Verb verb = parse_verb(params["verb"].str());
    const int target = params["target"].integer(0);
    if (!apply_compound(g_state, g_rules, tile, verb, target, &log)) {
      return error_response(id, "action refused");
    }
  }

  if (method == "advance_bots") {
    log = advance_bots(g_state, g_rules, 4000);
  }

  // The viewer defaults to the current player, so fog is applied from their perspective.
  // Default to the FIRST HUMAN SEAT, never to current_player. Defaulting to whoever is to
  // move means that the instant you end your turn, a StateView call without an explicit
  // player hands back the BOT's view of the map -- including everything it has explored and
  // you have not. Found by driving the UI, not by any test.
  PlayerId default_viewer = kNoPlayer;
  for (PlayerId p = 0; p < g_state.player_count; ++p) {
    if (g_state.players[p].is_human) {
      default_viewer = p;
      break;
    }
  }
  PlayerId viewer = static_cast<PlayerId>(params["player"].integer(default_viewer));
  if (viewer < 0 || viewer >= g_state.player_count) viewer = kNoPlayer;

  // The log lines apply_compound() and the bot loop produced. `act` needs them as much as
  // `advance_bots` does: a ruin reward is chosen by the ENGINE, so the only way the player
  // learns what came out of the ruins they just walked into is this array.
  const auto log_array = [&log]() {
    std::string out = "[";
    for (std::size_t i = 0; i < log.size(); ++i) {
      if (i != 0) out += ',';
      // Structured, with the sentence alongside. The browser can switch on `kind` instead of
      // regex-matching prose, which is what it had to do while the engine wrote the copy.
      out += event_json(log[i], g_rules);
    }
    out += ']';
    return out;
  };

  json::Writer w;
  w.begin_object();
  w.field("id", id);
  w.key("result");
  if (method == "advance_bots") {
    w.begin_object();
    w.key("state");
    w.end_object();
    // Assemble by hand: the state view is already-serialised text, not a tree.
    std::string body = w.text();
    body.resize(body.size() - 1);  // drop the closing brace of `result`
    body += state_view_json(g_state, g_rules, viewer);
    body += ",\"log\":" + log_array() + "}}";
    return body;
  }
  w.end_object();
  std::string body = w.text();
  body.resize(body.size() - 1);
  std::string view = state_view_json(g_state, g_rules, viewer);
  if (method == "act") {
    // A StateView with one extra key, not a new envelope: `act` still answers a StateView,
    // so nothing that already reads it has to change.
    view.resize(view.size() - 1);
    view += ",\"log\":" + log_array() + "}";
  }
  body += view;
  body += "}";
  return body;
}

int run_rpc() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    json::Value request;
    std::string error;
    if (!json::parse(line, request, error)) {
      std::cout << error_response(0, "malformed request: " + error) << std::endl;
      continue;
    }
    std::cout << handle_request(request) << std::endl;
  }
  return 0;
}

// The whole-engine smoke test: play complete games choosing uniformly at random from the
// legal set. It proves three things that no unit test can -- that a game always terminates,
// that the legal set is never empty for an ongoing game, and that no sequence of legal
// actions can reach an invalid state.
int run_selfplay(int games, int max_steps) {
  Rules rules = Rules::defaults();
  std::string error;
  if (load_rules(g_rules_path, rules, error)) {
    std::printf("rules: %s (%zu unverified)\n\n", rules.version.c_str(),
                rules.unverified.size());
  } else {
    std::printf("rules: built-in defaults (%s)\n\n", error.c_str());
  }

  int completed = 0;
  int stalled = 0;
  long long total_steps = 0;
  long long total_turns = 0;
  const auto start = std::chrono::steady_clock::now();

  for (int g = 0; g < games; ++g) {
    GameConfig cfg;
    cfg.seed = static_cast<std::uint64_t>(g + 1);
    cfg.player_count = 2;
    cfg.tribes[0] = Tribe::Imperius;
    cfg.tribes[1] = Tribe::Bardur;
    // A HARNESS bound, not a game rule. The game itself has no turn limit; agents choosing
    // uniformly at random essentially never achieve domination, so without this the smoke
    // test runs forever. Real games end by capture.
    cfg.turn_limit = 100;

    GameState s;
    if (!new_game(cfg, rules, s, error)) {
      std::printf("game %d: setup failed: %s\n", g, error.c_str());
      return 1;
    }

    int steps = 0;
    while (steps < max_steps && !outcome(s, rules).has_value()) {
      ActionMask mask;
      legal_actions(s, rules, mask);
      if (mask.empty()) {
        std::printf("game %d step %d: EMPTY LEGAL SET (player %d, phase %d)\n", g, steps,
                    s.current_player, static_cast<int>(s.phase));
        return 1;
      }
      std::int16_t slots[kActionSlots];
      const int count = mask.collect(slots, kActionSlots);
      const int pick = static_cast<int>(s.rng.bounded(static_cast<std::uint32_t>(count)));
      if (!apply(s, rules, Action{slots[pick]}, nullptr)) {
        std::printf("game %d step %d: engine offered an action it then rejected (%d)\n", g,
                    steps, slots[pick]);
        return 1;
      }
      ++steps;
    }

    total_steps += steps;
    total_turns += s.turn;
    const MaybeOutcome result = outcome(s, rules);
    if (result.has_value()) {
      ++completed;
    } else {
      ++stalled;
    }
  }

  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  std::printf("games         %d\n", games);
  std::printf("completed     %d\n", completed);
  std::printf("hit step cap  %d\n", stalled);
  std::printf("avg steps     %.1f\n", static_cast<double>(total_steps) / games);
  std::printf("avg turns     %.1f\n", static_cast<double>(total_turns) / games);
  std::printf("throughput    %.0f steps/sec\n", static_cast<double>(total_steps) / seconds);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string mode = "--help";
  int games = 20;
  std::string agent_a = "random";
  std::string agent_b = "osla";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--rpc" || arg == "--bench") {
      mode = arg;
    } else if (arg == "--audit") {
      mode = arg;
      if (i + 1 < argc && argv[i + 1][0] != '-') games = std::atoi(argv[++i]);
    } else if (arg == "--audit-seed") {
      // Selects the mode on its own. Requiring `--audit 1 --audit-seed N` meant the replay
      // instruction printed in every finding did not actually work.
      mode = "--audit";
    } else if (arg == "--selfplay") {
      mode = arg;
      if (i + 1 < argc) games = std::atoi(argv[++i]);
    } else if (arg == "--rules" && i + 1 < argc) {
      g_rules_path = argv[++i];
    } else if (arg == "--ladder") {
      mode = arg;
      if (i + 1 < argc && argv[i + 1][0] != '-') games = std::atoi(argv[++i]);
    } else if (arg == "--match") {
      mode = arg;
      if (i + 2 < argc) {
        agent_a = argv[++i];
        agent_b = argv[++i];
      }
    } else if (arg == "--games" && i + 1 < argc) {
      games = std::atoi(argv[++i]);
    }
  }

  if (mode == "--rpc") {
    std::string error;
    Rules loaded;
    if (load_rules(g_rules_path, loaded, error)) g_rules = loaded;
    return run_rpc();
  }
  if (mode == "--selfplay") return run_selfplay(games, 200000);
  if (mode == "--audit") {
    Rules rules = Rules::defaults();
    std::string error;
    load_rules(g_rules_path, rules, error);
    bool verbose = false;
    std::uint64_t only_seed = 0;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--verbose") verbose = true;
      if (arg == "--audit-seed" && i + 1 < argc) {
        only_seed = static_cast<std::uint64_t>(std::atoll(argv[++i]));
      }
    }
    if (only_seed != 0) return audit::run_seed(only_seed, rules);
    return audit::run(games, rules, verbose);
  }
  if (mode == "--ladder" || mode == "--match") {
    Rules rules = Rules::defaults();
    std::string error;
    load_rules(g_rules_path, rules, error);

    // Preset FIRST, so the individual flags below can still override fields of it. The default
    // is the game's own setup (16x16, four tribes, fog on); `--paper` asks for the 2020 AIIDE
    // format instead, and `--playtest` is accepted as an explicit no-op spelling of the
    // default for callers that would rather say it than assume it.
    agents::MatchConfig cfg;
    for (int i = 1; i < argc; ++i) {
      if (std::string(argv[i]) == "--paper") cfg = agents::MatchConfig::paper_2020();
    }
    cfg.games = games;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--budget" && i + 1 < argc) cfg.budget = std::atoi(argv[++i]);
      if (arg == "--turns" && i + 1 < argc) cfg.turn_limit = std::atoi(argv[++i]);
      if (arg == "--players" && i + 1 < argc) cfg.player_count = std::atoi(argv[++i]);
      if (arg == "--fog") cfg.fog = true;
      if (arg == "--no-fog") cfg.fog = false;
      if (arg == "--size" && i + 1 < argc) {
        const int n = std::atoi(argv[++i]);
        cfg.dims = MapDims{static_cast<std::int16_t>(n), static_cast<std::int16_t>(n)};
      }
      if (arg == "--seed" && i + 1 < argc) {
        cfg.seed = static_cast<std::uint64_t>(std::atoi(argv[++i]));
      }
    }
    std::printf("setup: %dx%d, %d players, fog %s, turn limit %d\n", cfg.dims.width,
                cfg.dims.height, cfg.player_count, cfg.fog ? "on" : "off", cfg.turn_limit);

    if (mode == "--match") {
      auto a = agents::make_agent(agent_a);
      auto b = agents::make_agent(agent_b);
      if (!a || !b) {
        std::printf("unknown agent (have: ");
        for (int i = 0; i < agents::agent_count(); ++i) {
          std::printf("%s ", agents::kAgentNames[i]);
        }
        std::printf(")\n");
        return 1;
      }
      std::printf("%s vs %s -- %d games, %d forward-model calls/decision\n\n", a->name(),
                  b->name(), cfg.games, cfg.budget);
      const agents::MatchResult r = agents::play_match(*a, *b, rules, cfg);
      std::printf("  %-10s %.1f%%   (%d-%d-%d)\n", a->name(), 100.0 * r.score_a(), r.wins_a,
                  r.wins_b, r.draws);
      std::printf("  mean turns %.1f, %.1fs, Elo diff %+.0f\n", r.mean_turns, r.seconds,
                  agents::elo_difference(r.score_a()));
      return 0;
    }

    std::vector<std::string> names;
    for (int i = 0; i < agents::agent_count(); ++i) names.emplace_back(agents::kAgentNames[i]);
    std::printf("round robin -- %d games per pairing, %d calls/decision\n\n", cfg.games,
                cfg.budget);
    const auto ratings = agents::round_robin(names, rules, cfg, "random");

    std::printf("\n  %-12s %8s %7s\n", "agent", "elo", "games");
    for (const auto& r : ratings) {
      std::printf("  %-12s %8.0f %7d\n", r.name.c_str(), r.elo, r.games);
    }
    std::printf("\n  Published 2020 ordering, which this must recover:\n");
    std::printf("  rhea > rulebased > mcts > mc > osla > random\n");
    return 0;
  }

  std::printf("polytopia engine %s\n\n", poly::engine_version());
  std::printf("  --rpc                JSON-RPC on stdin/stdout\n");
  std::printf("  --selfplay [N]       play N random-legal games (whole-engine smoke test)\n");
  std::printf("  --match <a> <b>      play two agents head to head\n");
  std::printf("  --ladder [N]         round robin over all baselines, N games per pairing\n");
  std::printf("  --games N            games per pairing\n");
  std::printf("  --budget N           forward-model calls per decision (default 2000)\n");
  std::printf("  --audit N            fuzz N games: invariants + a census of what never happens\n");
  std::printf("  --audit-seed N       replay one audited game with every check on\n");
  std::printf("  --paper              the 2020 AIIDE setup: 11x11, 2 players, fog off\n");
  std::printf("                       (default is the GAME setup: 16x16, 4 players, fog on)\n");
  std::printf("  --size N / --players N / --fog / --no-fog / --turns N\n");
  std::printf("  --turns N            turn limit per match game (default 60)\n");
  std::printf("  --rules <path>       rule data file (default data/rules/live.json)\n");
  std::printf("\n  agents: ");
  for (int i = 0; i < poly::agents::agent_count(); ++i) {
    std::printf("%s ", poly::agents::kAgentNames[i]);
  }
  std::printf("\n");
  return 0;
}
