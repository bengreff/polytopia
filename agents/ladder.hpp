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

// THE DEFAULT IS THE GAME'S OWN SETUP: 16x16, four tribes, fog on. See kDefaultMapDims.
//
// It used to be the 2020 paper's 11x11 two-player fog-off duel, which was wrong twice over.
// First, `play_one` hardcoded two players and the CLI had no flag for size, seats or fog, so
// the paper format was the ONLY thing the harness could express and the format people actually
// play went unmeasured. game.cpp's GameConfig comment predicted the cost precisely: "testing
// against the wrong default hides whole classes of problem (village density, multi-front war,
// naval relevance)." It was right -- `paper2020` rates 2nd in the duel and 6th in the real
// game, where it loses to `random`.
//
// Second, and the reason this is a DEFAULT rather than just a flag: making the real setup
// opt-in leaves the correct measurement as the one you have to remember to ask for. Anything
// measured on autopilot should be measured on the thing being shipped.
//
// The paper format remains fully supported via `paper_2020()` / `--paper`. It is no longer a
// gate in any case -- HANDOFF section 7 records that reproducing the published ordering was
// dropped deliberately, so its only remaining role is as a stable relative yardstick.
struct MatchConfig {
  int games{20};
  int budget{2000};  // forward-model calls per decision, as in the 2020 paper

  MapDims dims{kDefaultMapDims};
  int player_count{kDefaultPlayerCount};
  bool fog{true};

  // The real game has no turn limit; a harness must have one or a stalemate hangs the run.
  // This is a BACKSTOP, not a rule -- if matches routinely reach it, that is a finding about
  // the agents (they cannot close a game on a big map), not a number to raise.
  int turn_limit{100};

  std::uint64_t seed{1};
  Tribe tribe_a{Tribe::XinXi};
  Tribe tribe_b{Tribe::Imperius};

  // The 2020 AIIDE experimental setup: 11x11, two players, FULL OBSERVABILITY, 50-turn cap.
  // Fog off is not an oversight -- the published experiments ran without it, and turning it on
  // makes the numbers incomparable to the published table.
  static MatchConfig paper_2020() {
    MatchConfig cfg;
    cfg.dims = kCompetitiveMapDims;
    cfg.player_count = kCompetitivePlayerCount;
    cfg.fog = false;
    cfg.turn_limit = 50;
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
