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

struct MatchConfig {
  int games{20};
  int budget{2000};        // forward-model calls per decision, as in the 2020 paper
  int turn_limit{60};      // bounds a match; the GAME has no limit
  MapDims dims{11, 11};
  std::uint64_t seed{1};
  Tribe tribe_a{Tribe::XinXi};
  Tribe tribe_b{Tribe::Imperius};
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
