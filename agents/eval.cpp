#include "agents/eval.hpp"

#include <algorithm>

namespace poly::agents {

Features extract_features(const GameState& s, const Rules& rules, PlayerId player) {
  Features f;
  f.production = static_cast<float>(player_income(s, rules, player));
  f.score = static_cast<float>(player_score(s, rules, player));

  for (int i = 0; i < kTechCount; ++i) {
    if (s.players[player].has_tech(static_cast<Tech>(i))) f.technologies += 1.0f;
  }
  for (CityId c = 0; c < s.city_count; ++c) {
    if (s.cities[c].owner != player) continue;
    f.cities += 1.0f;
    f.city_levels += static_cast<float>(s.cities[c].level);
  }
  for (UnitId u = 0; u < s.unit_count; ++u) {
    const Unit& unit = s.units[u];
    if (unit.owner != player) continue;
    if (unit.alive()) f.units += 1.0f;
    // "Units defeated" is a per-unit kill tally, which the engine already tracks for
    // veterancy -- the same counter serves both, so the feature costs nothing to maintain.
    f.units_defeated += static_cast<float>(unit.kills);
  }
  return f;
}

float weighted_sum(const Features& f, const EvalWeights& w) {
  return f.production * w.production + f.technologies * w.technologies + f.score * w.score +
         f.cities * w.cities + f.units * w.units + f.units_defeated * w.units_defeated +
         f.city_levels * w.city_levels;
}

float evaluate(const GameState& s, const Rules& rules, PlayerId seat, const EvalWeights& w) {
  const float mine = weighted_sum(extract_features(s, rules, seat), w);

  // "Per-tribe progress values are averaged across opponents and subtracted from the root
  // player's." Averaging rather than summing keeps the scale independent of player count,
  // which matters because the same evaluator has to work in 1v1 and in free-for-alls.
  float opponent_total = 0.0f;
  int opponents = 0;
  for (PlayerId p = 0; p < s.player_count; ++p) {
    if (p == seat || !s.players[p].alive) continue;
    opponent_total += weighted_sum(extract_features(s, rules, p), w);
    ++opponents;
  }
  if (opponents == 0) return mine;
  return mine - opponent_total / static_cast<float>(opponents);
}

float evaluate_leaf(const GameState& s, const Rules& rules, PlayerId seat, const EvalWeights& w) {
  const MaybeOutcome result = outcome(s, rules);
  if (result.has_value()) {
    // A decided game saturates. kWinValue is unreachable by the heuristic, so a real win can
    // never be confused with a merely good position -- the separation is structural, not a
    // hope that the numbers do not overlap.
    if (result->is_draw()) return 0.0f;
    return result->winner == seat ? kWinValue : -kWinValue;
  }
  // No outcome means the game is ongoing. A search that stops here is at a HORIZON, not a
  // terminal, and gets the heuristic -- which is exactly the distinction outcome.hpp exists
  // to make unrepresentable-as-equal.
  return evaluate(s, rules, seat, w);
}

}  // namespace poly::agents
