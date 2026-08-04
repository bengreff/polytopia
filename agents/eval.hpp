// The handcrafted position evaluation.
//
// This is a faithful reproduction of the 2020 paper's function, which is the published state
// of the art for this game: seven linear features with hand-set weights. Reproducing it
// exactly is the point -- it is the baseline Phase 2 has to beat by 200 Elo, and a baseline
// you have quietly improved is not a baseline.
//
// From docs/research-findings.md §1:
//   Features: deltas in production, technologies researched, game score, cities owned,
//             units owned, units defeated, and sum of owned city levels.
//   Weights:  W = {5, 4, 0.1, 4, 2, 3, 2}
//   "Per-tribe progress values are averaged across opponents and subtracted from the root
//    player's."
//
// That is the entire published SOTA position evaluator for this game. It is a very low bar,
// and saying so is the reason Phase 2 exists.

#pragma once

#include "poly/game.hpp"

namespace poly::agents {

struct EvalWeights {
  // Order matches the paper's W.
  float production{5.0f};
  float technologies{4.0f};
  float score{0.1f};
  float cities{4.0f};
  float units{2.0f};
  float units_defeated{3.0f};
  float city_levels{2.0f};
};

// The seven raw features for one player, before weighting.
struct Features {
  float production{0.0f};
  float technologies{0.0f};
  float score{0.0f};
  float cities{0.0f};
  float units{0.0f};
  float units_defeated{0.0f};
  float city_levels{0.0f};
};

Features extract_features(const GameState& s, const Rules& rules, PlayerId player);

float weighted_sum(const Features& f, const EvalWeights& w);

// The paper's evaluation: the root player's progress minus the mean of the opponents'.
//
// Returns a scalar FOR A NAMED SEAT. It is never "the value of this position" in the
// abstract -- the seat is a parameter precisely so no caller can derive perspective from
// tree depth (README §3 avoid-item 2).
float evaluate(const GameState& s, const Rules& rules, PlayerId seat, const EvalWeights& w);

// Terminal-aware evaluation for search leaves.
//
// A DECIDED game returns a saturating win/loss magnitude; an ongoing position returns the
// heuristic. These must never collide: Polyfish returned 0.0 for both terminal states and
// horizon cutoffs, which made "capturing the enemy capital look identical to a draw inside
// the search" (docs/research-findings.md §4). Here a win is +kWinValue, unreachable by the
// heuristic, so the two are separated by construction rather than by hoping the numbers
// happen not to overlap.
inline constexpr float kWinValue = 1e6f;

float evaluate_leaf(const GameState& s, const Rules& rules, PlayerId seat, const EvalWeights& w);

}  // namespace poly::agents
