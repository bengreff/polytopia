// The agent interface, and the forward-model budget every search agent spends against.
//
// Phase 1 (README §5) reproduces the 2020 AIIDE baselines in-engine so the ladder has anchors:
// Random, OSLA, Monte Carlo, MCTS, RHEA, plus a rule-based agent. The gate is not "these
// exist" but that they RECOVER THE PUBLISHED ORDERING:
//
//     RHEA 74.84% > RuleBased 65.80% > MCTS 60.44% > MC 50.32% > OSLA 47.76% > Random 0.84%
//
// That is the strongest whole-engine correctness check available. Six independent agents
// ranking differently here than in the paper means something in the rules is wrong, and we
// find out before any of it matters.

#pragma once

#include <cstdint>
#include <string>

#include "poly/game.hpp"

namespace poly::agents {

// A forward model with a spend limit.
//
// The paper budgets 2000 next() calls per decision rather than wall-clock, so results are
// hardware-independent and reproducible. Copying that exactly is what makes our numbers
// comparable to theirs -- a wall-clock budget would make the comparison meaningless.
class ForwardModel {
 public:
  ForwardModel(const Rules& rules, int budget) : rules_(rules), budget_(budget) {}

  [[nodiscard]] bool exhausted() const { return spent_ >= budget_; }
  [[nodiscard]] int spent() const { return spent_; }
  [[nodiscard]] int remaining() const { return budget_ - spent_; }
  void reset() { spent_ = 0; }

  // The ONLY way an agent may advance a state. Returns false when the budget is gone, so a
  // search that ignores the return value degrades into "stop expanding" rather than
  // silently overspending.
  bool step(GameState& s, Action action, const ActionMask* mask = nullptr) {
    if (exhausted()) return false;
    ++spent_;
    return apply(s, rules_, action, nullptr, mask);
  }

  [[nodiscard]] const Rules& rules() const { return rules_; }

 private:
  const Rules& rules_;
  int budget_;
  int spent_{0};
};

class Agent {
 public:
  virtual ~Agent() = default;

  // Chooses one action from `mask`. The mask is never empty for an ongoing game.
  //
  // `seat` is passed explicitly and is NOT inferred from the state, let alone from search
  // depth. Polytopia allows consecutive same-player moves, and inferring perspective from
  // depth parity is the documented bug that crippled Polyfish's search
  // (docs/research-findings.md §4). Every agent here evaluates from a named seat.
  virtual Action choose(const GameState& s, ForwardModel& model, const ActionMask& mask,
                        PlayerId seat) = 0;

  [[nodiscard]] virtual const char* name() const = 0;

  // Agents are stateless across games but carry their own RNG so a match is reproducible
  // from its seed alone.
  virtual void seed(std::uint64_t value) { rng_.seed(value); }

 protected:
  Pcg32 rng_;
};

// Picks uniformly from a mask using the agent's own generator.
Action sample_uniform(const ActionMask& mask, Pcg32& rng);

}  // namespace poly::agents
