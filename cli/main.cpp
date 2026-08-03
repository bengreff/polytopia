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
#include "poly/json.hpp"
#include "poly/version.hpp"

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

std::string error_response(int id, const std::string& message) {
  json::Writer w;
  w.begin_object();
  w.field("id", id);
  w.field("error", message);
  w.end_object();
  return w.text();
}

// Runs every non-human player until it is a human's decision again, or the game ends.
// Currently plays random-legal; bots/scripted.cpp replaces this with the in-game-bot clone.
std::vector<std::string> advance_bots(GameState& s, const Rules& rules, int step_budget) {
  std::vector<std::string> log;
  int steps = 0;
  while (steps++ < step_budget && !outcome(s, rules).has_value()) {
    if (s.players[s.current_player].is_human) break;
    ActionMask mask;
    legal_actions(s, rules, mask);
    if (mask.empty()) break;
    std::int16_t slots[kActionSlots];
    const int count = mask.collect(slots, kActionSlots);
    if (count == 0) break;
    const int pick = static_cast<int>(s.rng.bounded(static_cast<std::uint32_t>(count)));
    if (!apply(s, rules, Action{slots[pick]}, &log)) break;
  }
  return log;
}

std::string handle_request(const json::Value& request) {
  const int id = request["id"].integer();
  const std::string method = request["method"].str();
  const json::Value& params = request["params"];

  if (method == "new_game") {
    GameConfig cfg;
    cfg.dims.width = static_cast<std::int16_t>(params["width"].integer(11));
    cfg.dims.height = static_cast<std::int16_t>(params["height"].integer(11));
    cfg.seed = static_cast<std::uint64_t>(params["seed"].integer(1));
    cfg.mode = parse_mode(params["mode"].str());
    cfg.turn_limit = params["turn_limit"].integer(0);
    cfg.fog = params["fog"].boolean(true);
    cfg.land_ratio = static_cast<float>(params["land_ratio"].num(0.6));

    const auto& players = params["players"].arr();
    cfg.player_count = static_cast<int>(players.size());
    if (cfg.player_count < 2) {
      cfg.player_count = 2;
      cfg.tribes[0] = Tribe::Imperius;
      cfg.tribes[1] = Tribe::Bardur;
      cfg.is_human[0] = true;
    } else {
      for (int i = 0; i < cfg.player_count && i < kMaxPlayers; ++i) {
        cfg.tribes[i] = parse_tribe(players[static_cast<std::size_t>(i)]["tribe"].str(),
                                    static_cast<Tribe>(i % kTribeCount));
        cfg.is_human[i] = players[static_cast<std::size_t>(i)]["human"].boolean(i == 0);
        cfg.difficulty[i] =
            parse_difficulty(players[static_cast<std::size_t>(i)]["difficulty"].str());
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

  std::vector<std::string> log;
  if (method == "advance_bots") {
    log = advance_bots(g_state, g_rules, 4000);
  }

  // The viewer defaults to the current player, so fog is applied from their perspective.
  PlayerId viewer = static_cast<PlayerId>(params["player"].integer(g_state.current_player));
  if (viewer < 0 || viewer >= g_state.player_count) viewer = kNoPlayer;

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
    body += ",\"log\":[";
    for (std::size_t i = 0; i < log.size(); ++i) {
      if (i != 0) body += ',';
      json::Writer escaped;
      escaped.value(log[i]);
      body += escaped.text();
    }
    body += "]}}";
    return body;
  }
  w.end_object();
  std::string body = w.text();
  body.resize(body.size() - 1);
  body += state_view_json(g_state, g_rules, viewer);
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
    cfg.turn_limit = 60;

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
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--rpc" || arg == "--bench") {
      mode = arg;
    } else if (arg == "--selfplay") {
      mode = arg;
      if (i + 1 < argc) games = std::atoi(argv[++i]);
    } else if (arg == "--rules" && i + 1 < argc) {
      g_rules_path = argv[++i];
    }
  }

  if (mode == "--rpc") {
    std::string error;
    Rules loaded;
    if (load_rules(g_rules_path, loaded, error)) g_rules = loaded;
    return run_rpc();
  }
  if (mode == "--selfplay") return run_selfplay(games, 200000);

  std::printf("polytopia engine %s\n\n", poly::engine_version());
  std::printf("  --rpc                JSON-RPC on stdin/stdout\n");
  std::printf("  --selfplay [N]       play N random-legal games (whole-engine smoke test)\n");
  std::printf("  --rules <path>       rule data file (default data/rules/live.json)\n");
  return 0;
}
