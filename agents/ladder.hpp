// Match running and Elo.
//
// README §5 puts this FIRST, before any search work: "Nothing else is trusted without it."
// The defining failure of this field is shipping agents with no strength measurement --
// Polyfish's own expert review led with "you have no strength measurement", and tribes-rl
// shipped an excellent engine with completely unknown agent strength.

#pragma once

#include <string>
#include <vector>

#include "agents/agent.hpp"

namespace poly::agents {

// THESE DEFAULTS ARE THE 2020 PAPER'S FORMAT, NOT THE GAME'S.
//
// 11x11, two players, full observability, 50-turn cap. That is deliberate and must stay --
// the Phase 1 anchors are only comparable to the published table under the published setup.
//
// But it is NOT what `GameConfig` gives you when you press New Game (16x16, four tribes, fog
// on, no turn limit), and for a long time it was the ONLY setup the harness could express:
// `play_one` hardcoded two players, and the CLI had no flag for size, seats or fog. So every
// measured number described a format nobody plays, and the format people do play was
// unmeasured. game.cpp's own GameConfig comment predicted the cost of exactly this: "testing
// against the wrong default hides whole classes of problem (village density, multi-front war,
// naval relevance)."
//
// `playtest_default()` below is the other setup. Measure in both.
struct MatchConfig {
  int games{20};
  int budget{2000};        // forward-model calls per decision, as in the 2020 paper
  int turn_limit{50};      // bounds a match; the GAME has no limit. The paper used 50.
  // The 2020 experiments ran with FULL OBSERVABILITY. Leaving fog on makes our numbers
  // incomparable to the published table, which is the one thing the Phase 1 gate depends on.
  bool fog{false};
  MapDims dims{11, 11};
  std::uint64_t seed{1};
  Tribe tribe_a{Tribe::XinXi};
  Tribe tribe_b{Tribe::Imperius};

  // Seats beyond the first are filled by agent B, so `play_match(a, b, ...)` reads as "one A
  // against a field of B" -- which is the shape of the thing being playtested: a human on one
  // seat against three bots.
  int player_count{2};

  // What a player actually gets from New Game. Keep in lockstep with GameConfig.
  static MatchConfig playtest_default() {
    MatchConfig cfg;
    cfg.dims = MapDims{16, 16};
    cfg.player_count = 4;
    cfg.fog = true;
    // The real game has no limit; a harness must have one or a stalemate hangs the run. This
    // is a BACKSTOP, not a rule -- if matches routinely reach it, that is a finding about the
    // agents, not a setting to raise.
    cfg.turn_limit = 100;
    return cfg;
  }
};

struct MatchResult {
  int games{0};
  int wins_a{0};
  int wins_b{0};
  int draws{0};
  double mean_turns{0.0};
  double seconds{0.0};

  [[nodiscard]] double score_a() const {
    if (games == 0) return 0.0;
    return (wins_a + 0.5 * draws) / games;
  }
};

// Plays `cfg.games` games, alternating seats so map advantage cannot be mistaken for skill.
// Each game uses a distinct seed derived from cfg.seed, so a match is reproducible entirely
// from its config.
MatchResult play_match(Agent& a, Agent& b, const Rules& rules, const MatchConfig& cfg);

// Elo from a score rate, relative to an opponent's rating. Returns +/-inf-ish bounds clamped,
// because a 0% or 100% result carries no finite rating information and pretending otherwise
// is how bogus ladder numbers get published.
double elo_difference(double score_rate);

struct Rating {
  std::string name;
  double elo{1000.0};
  int games{0};
};

// A full round robin. Ratings are fitted by iterative logistic regression against the
// observed pairwise scores, with `anchor` pinned so the scale is stable across runs -- a
// ladder whose absolute numbers drift between runs cannot detect a regression.
std::vector<Rating> round_robin(const std::vector<std::string>& agent_names, const Rules& rules,
                                const MatchConfig& cfg, const std::string& anchor);

}  // namespace poly::agents
