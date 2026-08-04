#include "agents/ladder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

#include "agents/registry.hpp"

namespace poly::agents {
namespace {

// One game. `seat_of_a` says which side agent A plays, so the caller can alternate.
struct GameOutcome {
  PlayerId winner{kNoPlayer};
  int turns{0};
};

GameOutcome play_one(Agent& a, Agent& b, const Rules& rules, const MatchConfig& cfg,
                     std::uint64_t seed, PlayerId seat_of_a) {
  GameConfig setup;
  setup.dims = cfg.dims;
  setup.seed = seed;
  setup.player_count = 2;
  setup.turn_limit = cfg.turn_limit;
  setup.fog = cfg.fog;
  setup.tribes[0] = cfg.tribe_a;
  setup.tribes[1] = cfg.tribe_b;

  GameState s;
  std::string error;
  GameOutcome result;
  if (!new_game(setup, rules, s, error)) return result;

  // Each agent is reseeded per game so a match replays identically from its config.
  a.seed(seed * 2654435761u + 1u);
  b.seed(seed * 2654435761u + 2u);

  // The phase machine permits NO-OP CYCLES: Select then Deselect costs two decisions and
  // changes nothing, so an agent can burn an unbounded number of decisions inside one turn
  // without ever ending it. Measured at ~2400 decisions/turn for MCTS, which pinned games at
  // turn 5 against the step ceiling and recorded them as draws.
  //
  // The 2020 paper hit this too and reports that "forcing an EndTurn every X steps was tried
  // and discarded". We force it only as a HARNESS backstop, well above normal play, so it
  // never binds on a healthy agent but stops a pathological one from stalling the ladder.
  const int max_steps = cfg.turn_limit * 400;
  const int max_decisions_per_turn = 300;
  int decisions_this_turn = 0;
  int turn_at_last_decision = s.turn;
  PlayerId mover_at_last_decision = s.current_player;

  for (int step = 0; step < max_steps; ++step) {
    if (outcome(s, rules).has_value()) break;
    ActionMask mask;
    legal_actions(s, rules, mask);
    if (mask.empty()) break;

    if (s.turn != turn_at_last_decision || s.current_player != mover_at_last_decision) {
      decisions_this_turn = 0;
      turn_at_last_decision = s.turn;
      mover_at_last_decision = s.current_player;
    }
    if (++decisions_this_turn > max_decisions_per_turn) {
      // Drive the phase machine to EndTurn directly rather than asking the agent again.
      if (s.phase != Phase::Select) {
        apply(s, rules, Action{static_cast<std::int16_t>(Verb::Deselect)}, nullptr);
      }
      apply(s, rules, Action{static_cast<std::int16_t>(kGlobalActorSlot)}, nullptr);
      apply(s, rules, Action{static_cast<std::int16_t>(Verb::EndTurn)}, nullptr);
      continue;
    }

    Agent& mover = (s.current_player == seat_of_a) ? a : b;
    // A FRESH budget per decision, which is what the paper's 2000-call figure means.
    ForwardModel model(rules, cfg.budget);
    const Action action = mover.choose(s, model, mask, s.current_player);

    // The agent may return something illegal if it is buggy or ran out of budget mid-search.
    // Falling back keeps the match running instead of aborting the whole ladder, and is
    // visible because a broken agent will simply rate badly.
    if (!mask.test(action.index)) {
      Pcg32 fallback;
      fallback.seed(seed + static_cast<std::uint64_t>(step));
      apply(s, rules, sample_uniform(mask, fallback), nullptr);
    } else {
      apply(s, rules, action, nullptr);
    }
  }

  const MaybeOutcome final_state = outcome(s, rules);
  result.turns = s.turn;
  if (final_state.has_value()) {
    result.winner = final_state->winner;
    return result;
  }
  // Ran out of steps without a decision. Settle on SCORE rather than recording a draw --
  // scoring an unfinished game as drawn silently rewards an agent for stalling, which is
  // exactly what a broken search does.
  int best = -1;
  for (PlayerId p = 0; p < s.player_count; ++p) {
    const int score = player_score(s, rules, p);
    if (score > best) {
      best = score;
      result.winner = p;
    } else if (score == best) {
      result.winner = kNoPlayer;
    }
  }
  return result;
}

double expected_score(double rating, double opponent) {
  return 1.0 / (1.0 + std::pow(10.0, (opponent - rating) / 400.0));
}

}  // namespace

MatchResult play_match(Agent& a, Agent& b, const Rules& rules, const MatchConfig& cfg) {
  MatchResult result;
  const auto start = std::chrono::steady_clock::now();
  long long total_turns = 0;

  for (int g = 0; g < cfg.games; ++g) {
    // Alternate seats. Without this, any map-generation asymmetry between seat 0 and seat 1
    // is indistinguishable from a skill difference -- and seat 0 moves first, which is a real
    // advantage in this game.
    const PlayerId seat_of_a = static_cast<PlayerId>(g % 2);
    const std::uint64_t seed = cfg.seed + static_cast<std::uint64_t>(g);
    const GameOutcome outcome_of_game = play_one(a, b, rules, cfg, seed, seat_of_a);

    ++result.games;
    total_turns += outcome_of_game.turns;
    if (outcome_of_game.winner == kNoPlayer) {
      ++result.draws;
    } else if (outcome_of_game.winner == seat_of_a) {
      ++result.wins_a;
    } else {
      ++result.wins_b;
    }
  }

  result.mean_turns = result.games ? static_cast<double>(total_turns) / result.games : 0.0;
  result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  return result;
}

double elo_difference(double score_rate) {
  // A 0% or 100% result carries no finite rating information. Clamping rather than returning
  // infinity keeps the ladder printable, but the clamp is deliberately wide so a saturated
  // pairing is obvious rather than looking like a merely large gap.
  const double clamped = std::clamp(score_rate, 0.001, 0.999);
  return -400.0 * std::log10(1.0 / clamped - 1.0);
}

std::vector<Rating> round_robin(const std::vector<std::string>& agent_names, const Rules& rules,
                                const MatchConfig& cfg, const std::string& anchor) {
  const std::size_t n = agent_names.size();
  std::vector<Rating> ratings(n);
  for (std::size_t i = 0; i < n; ++i) ratings[i].name = agent_names[i];

  // Observed pairwise scores.
  std::vector<std::vector<double>> score(n, std::vector<double>(n, 0.0));
  std::vector<std::vector<int>> played(n, std::vector<int>(n, 0));

  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = i + 1; j < n; ++j) {
      auto a = make_agent(agent_names[i]);
      auto b = make_agent(agent_names[j]);
      if (!a || !b) continue;
      const MatchResult r = play_match(*a, *b, rules, cfg);
      score[i][j] = r.score_a();
      score[j][i] = 1.0 - r.score_a();
      played[i][j] = played[j][i] = r.games;
      ratings[i].games += r.games;
      ratings[j].games += r.games;
      std::printf("  %-10s vs %-10s  %.1f%%  (%d-%d-%d, %.1f turns, %.1fs)\n",
                  agent_names[i].c_str(), agent_names[j].c_str(), 100.0 * r.score_a(),
                  r.wins_a, r.wins_b, r.draws, r.mean_turns, r.seconds);
    }
  }

  // Fit ratings by gradient ascent on the logistic likelihood. Simple, and adequate for a
  // handful of agents; the point is a consistent scale, not statistical elegance.
  for (int iteration = 0; iteration < 4000; ++iteration) {
    std::vector<double> gradient(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
      for (std::size_t j = 0; j < n; ++j) {
        if (i == j || played[i][j] == 0) continue;
        const double expected = expected_score(ratings[i].elo, ratings[j].elo);
        gradient[i] += static_cast<double>(played[i][j]) * (score[i][j] - expected);
      }
    }
    for (std::size_t i = 0; i < n; ++i) ratings[i].elo += 0.5 * gradient[i];

    // Pin the anchor so the scale does not drift between runs. A ladder whose absolute
    // numbers move every run cannot detect a regression, only a reordering.
    double shift = 0.0;
    for (const Rating& r : ratings) {
      if (r.name == anchor) shift = 1000.0 - r.elo;
    }
    for (Rating& r : ratings) r.elo += shift;
  }

  std::sort(ratings.begin(), ratings.end(),
            [](const Rating& a, const Rating& b) { return a.elo > b.elo; });
  return ratings;
}

}  // namespace poly::agents
