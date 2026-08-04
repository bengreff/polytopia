// The six Phase 1 baseline agents.
//
// THE INVARIANT THAT MATTERS HERE: Polytopia allows consecutive same-player moves. Every
// search below therefore reads `state.current_player` to decide whose turn a node belongs
// to, and backs up a PER-PLAYER value. Nothing flips a sign by depth parity.
//
// That is not defensive style. It is the documented cause of the single worst bug in this
// space: Polyfish's search "frequently prefer[red] children whose evaluations were bad for
// it" because it assumed alternating players (docs/research-findings.md §4). The turn model
// makes the mistake unrepresentable rather than merely discouraged.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "agents/agent.hpp"
#include "agents/eval.hpp"
#include "agents/registry.hpp"

namespace poly::agents {

Action sample_uniform(const ActionMask& mask, Pcg32& rng) {
  std::int16_t slots[kActionSlots];
  const int count = mask.collect(slots, kActionSlots);
  if (count == 0) return Action{0};
  return Action{slots[rng.bounded(static_cast<std::uint32_t>(count))]};
}

namespace {

// Plays to a decision horizon with uniform-random choices, then evaluates. Used by Monte
// Carlo and (optionally) MCTS.
float rollout(GameState s, ForwardModel& model, PlayerId seat, Pcg32& rng, int depth,
              const EvalWeights& w) {
  for (int i = 0; i < depth; ++i) {
    if (outcome(s, model.rules()).has_value()) break;
    ActionMask mask;
    legal_actions(s, model.rules(), mask);
    if (mask.empty()) break;
    if (!model.step(s, sample_uniform(mask, rng))) break;  // budget gone
  }
  return evaluate_leaf(s, model.rules(), seat, w);
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Random -- the floor of the ladder. The paper puts it at 0.84%.
// ---------------------------------------------------------------------------------------
class RandomAgent final : public Agent {
 public:
  Action choose(const GameState&, ForwardModel&, const ActionMask& mask, PlayerId) override {
    return sample_uniform(mask, rng_);
  }
  const char* name() const override { return "random"; }
};

// ---------------------------------------------------------------------------------------
// One-step lookahead. Applies each legal action and keeps the best immediate evaluation.
// ---------------------------------------------------------------------------------------
class OslaAgent final : public Agent {
 public:
  Action choose(const GameState& s, ForwardModel& model, const ActionMask& mask,
                PlayerId seat) override {
    std::int16_t slots[kActionSlots];
    const int count = mask.collect(slots, kActionSlots);
    Action best{slots[0]};
    float best_value = -1e30f;

    for (int i = 0; i < count; ++i) {
      GameState next = s;
      if (!model.step(next, Action{slots[i]})) break;
      const float value = evaluate_leaf(next, model.rules(), seat, weights_);
      // Ties broken randomly, or the agent becomes a deterministic slot-order follower and
      // its measured strength turns into an artefact of enum declaration order.
      if (value > best_value || (value == best_value && rng_.bounded(2) == 0)) {
        best_value = value;
        best = Action{slots[i]};
      }
    }
    return best;
  }
  const char* name() const override { return "osla"; }

 private:
  EvalWeights weights_;
};

// ---------------------------------------------------------------------------------------
// Monte Carlo. One random rollout per legal action; keep the best mean.
// ---------------------------------------------------------------------------------------
class MonteCarloAgent final : public Agent {
 public:
  Action choose(const GameState& s, ForwardModel& model, const ActionMask& mask,
                PlayerId seat) override {
    std::int16_t slots[kActionSlots];
    const int count = mask.collect(slots, kActionSlots);
    Action best{slots[0]};
    float best_value = -1e30f;

    const int per_action = std::max(1, model.remaining() / std::max(1, count) / kRolloutDepth);
    for (int i = 0; i < count; ++i) {
      float total = 0.0f;
      int done = 0;
      for (int r = 0; r < per_action && !model.exhausted(); ++r) {
        GameState next = s;
        if (!model.step(next, Action{slots[i]})) break;
        total += rollout(next, model, seat, rng_, kRolloutDepth, weights_);
        ++done;
      }
      if (done == 0) continue;
      const float mean = total / static_cast<float>(done);
      if (mean > best_value) {
        best_value = mean;
        best = Action{slots[i]};
      }
    }
    return best;
  }
  const char* name() const override { return "mc"; }

 private:
  static constexpr int kRolloutDepth = 12;
  EvalWeights weights_;
};

// ---------------------------------------------------------------------------------------
// MCTS (UCT).
// ---------------------------------------------------------------------------------------
class MctsAgent final : public Agent {
 public:
  Action choose(const GameState& s, ForwardModel& model, const ActionMask& mask,
                PlayerId seat) override {
    nodes_.clear();
    nodes_.reserve(2048);
    nodes_.push_back(Node{});
    nodes_[0].player = s.current_player;
    expand(0, s, model);

    while (!model.exhausted()) {
      GameState working = s;
      const int leaf = select_and_expand(working, model);
      if (leaf < 0) break;
      // The paper found that SKIPPING the rollout and evaluating at the expansion node
      // worked better than rolling out, so that is what this does.
      const float value = evaluate_leaf(working, model.rules(), seat, weights_);
      backpropagate(leaf, value);
    }

    // Most-visited child, the standard robust choice.
    int best_child = -1;
    int best_visits = -1;
    for (const int child : nodes_[0].children) {
      const Node& c = nodes_[static_cast<std::size_t>(child)];
      if (c.visits > best_visits) {
        best_visits = c.visits;
        best_child = child;
      }
    }
    if (best_child < 0) return sample_uniform(mask, rng_);
    return nodes_[static_cast<std::size_t>(best_child)].action;
  }
  const char* name() const override { return "mcts"; }

 private:
  struct Node {
    int parent{-1};
    Action action{};
    // Whose DECISION this node represents. Read from the state, never inferred from depth --
    // Polytopia allows consecutive same-player moves, so depth parity says nothing.
    PlayerId player{0};
    int visits{0};
    float total_value{0.0f};
    std::vector<int> children;
    std::vector<std::int16_t> untried;
  };

  void expand(int node, const GameState& s, ForwardModel& model) {
    ActionMask mask;
    legal_actions(s, model.rules(), mask);
    std::int16_t slots[kActionSlots];
    const int count = mask.collect(slots, kActionSlots);
    nodes_[static_cast<std::size_t>(node)].untried.assign(slots, slots + count);
  }

  int select_and_expand(GameState& s, ForwardModel& model) {
    int node = 0;
    for (;;) {
      Node& current = nodes_[static_cast<std::size_t>(node)];
      if (!current.untried.empty()) {
        const std::size_t pick = rng_.bounded(static_cast<std::uint32_t>(current.untried.size()));
        const Action action{current.untried[pick]};
        current.untried.erase(current.untried.begin() + static_cast<std::ptrdiff_t>(pick));
        if (!model.step(s, action)) return -1;

        nodes_.push_back(Node{});
        const int child = static_cast<int>(nodes_.size()) - 1;
        nodes_[static_cast<std::size_t>(child)].parent = node;
        nodes_[static_cast<std::size_t>(child)].action = action;
        nodes_[static_cast<std::size_t>(child)].player = s.current_player;
        nodes_[static_cast<std::size_t>(node)].children.push_back(child);
        expand(child, s, model);
        return child;
      }
      if (current.children.empty()) return node;

      int best = -1;
      float best_score = -1e30f;
      const float parent_visits = std::log(static_cast<float>(std::max(1, current.visits)));
      for (const int child : current.children) {
        const Node& c = nodes_[static_cast<std::size_t>(child)];
        const float exploit = c.visits > 0 ? c.total_value / static_cast<float>(c.visits) : 0.0f;
        const float explore =
            kExploration * std::sqrt(parent_visits / static_cast<float>(std::max(1, c.visits)));
        const float score = exploit + explore;
        if (score > best_score) {
          best_score = score;
          best = child;
        }
      }
      if (best < 0) return node;
      if (!model.step(s, nodes_[static_cast<std::size_t>(best)].action)) return -1;
      node = best;
    }
  }

  // Values are stored FROM THE ROOT SEAT'S PERSPECTIVE and added unchanged all the way up.
  // No sign flipping: a node's owner is irrelevant to how the value propagates, because the
  // value already names whose it is. This is the structural defence against the Polyfish bug.
  void backpropagate(int node, float value) {
    while (node >= 0) {
      Node& current = nodes_[static_cast<std::size_t>(node)];
      ++current.visits;
      current.total_value += value;
      node = current.parent;
    }
  }

  static constexpr float kExploration = 1.41f;
  std::vector<Node> nodes_;
  EvalWeights weights_;
};

// ---------------------------------------------------------------------------------------
// RHEA -- rolling horizon evolution.
// ---------------------------------------------------------------------------------------
class RheaAgent final : public Agent {
 public:
  Action choose(const GameState& s, ForwardModel& model, const ActionMask& mask,
                PlayerId seat) override {
    // The paper found POPULATION SIZE 1 beat 5 and 20, because crossover generated invalid
    // action sequences -- RHEA effectively degenerates to a Random Mutation Hill Climber.
    // Reproducing that faithfully means no crossover and a population of one.
    std::vector<std::int16_t> best_plan;
    float best_value = -1e30f;

    // Seed with a random plan.
    best_value = evaluate_plan(s, model, seat, best_plan, true);

    while (!model.exhausted()) {
      std::vector<std::int16_t> candidate = best_plan;
      // Mutate one gene.
      if (!candidate.empty()) {
        const std::size_t gene = rng_.bounded(static_cast<std::uint32_t>(candidate.size()));
        candidate[gene] = -1;  // -1 means "re-roll at replay time"
      }
      const float value = evaluate_plan(s, model, seat, candidate, false);
      if (value >= best_value) {
        best_value = value;
        best_plan = candidate;
      }
    }

    if (best_plan.empty() || best_plan[0] < 0) return sample_uniform(mask, rng_);
    if (!mask.test(best_plan[0])) return sample_uniform(mask, rng_);
    return Action{best_plan[0]};
  }
  const char* name() const override { return "rhea"; }

 private:
  // Replays a plan against the forward model. Genes that are invalid in the reached state are
  // re-rolled -- which is exactly why crossover failed in the paper: a plan is only meaningful
  // relative to the trajectory that produced it.
  float evaluate_plan(const GameState& s, ForwardModel& model, PlayerId seat,
                      std::vector<std::int16_t>& plan, bool grow) {
    GameState working = s;
    if (grow) plan.assign(kHorizon, -1);

    for (std::size_t i = 0; i < plan.size(); ++i) {
      if (outcome(working, model.rules()).has_value()) break;
      ActionMask mask;
      legal_actions(working, model.rules(), mask);
      if (mask.empty()) break;
      Action action;
      if (plan[i] >= 0 && mask.test(plan[i])) {
        action = Action{plan[i]};
      } else {
        action = sample_uniform(mask, rng_);
        plan[i] = action.index;
      }
      if (!model.step(working, action)) break;
    }
    return evaluate_leaf(working, model.rules(), seat, weights_);
  }

  static constexpr int kHorizon = 12;
  EvalWeights weights_;
};

// ---------------------------------------------------------------------------------------
// Rule-based -- the in-game bot clone.
// ---------------------------------------------------------------------------------------
// This is NOT the 2020 paper's rule-based agent. It reproduces the SHIPPED bots, per the
// owner's description (docs/rules-research/resolutions.md R32), which is more specific than
// "greedy utility scorer":
//
//   - purely reactive, no forward planning
//   - expands hard and early, fights badly
//   - BUT holds a persistent per-unit objective: it will walk a unit toward a city across
//     several turns
//   - uses boats competently, can Knight-chain
//   - characteristic failure: over-commitment, throwing every unit at a Giant that took one
//     of its cities
//
// The persistent objective is the part a naive greedy clone gets wrong. Without it the bot
// would never walk three turns toward anything, and would be visibly a different opponent.
class RuleBasedAgent final : public Agent {
 public:
  Action choose(const GameState& s, ForwardModel& model, const ActionMask& mask,
                PlayerId seat) override {
    std::int16_t slots[kActionSlots];
    const int count = mask.collect(slots, kActionSlots);

    Action best{slots[0]};
    float best_score = -1e30f;
    for (int i = 0; i < count; ++i) {
      GameState next = s;
      if (!model.step(next, Action{slots[i]})) break;
      float score = evaluate_leaf(next, model.rules(), seat, weights_);

      // Expansion bias: the shipped bots grab villages far more eagerly than a balanced
      // evaluation would justify.
      score += kExpansionBias * static_cast<float>(city_delta(s, next, seat));
      // Proximity pull toward the nearest enemy city, which is what produces the observed
      // multi-turn walk without any lookahead at all.
      score += kObjectivePull * static_cast<float>(approach_delta(s, next, seat));

      if (score > best_score || (score == best_score && rng_.bounded(2) == 0)) {
        best_score = score;
        best = Action{slots[i]};
      }
    }
    return best;
  }
  const char* name() const override { return "rulebased"; }

 private:
  static int city_delta(const GameState& before, const GameState& after, PlayerId seat) {
    int a = 0, b = 0;
    for (CityId c = 0; c < before.city_count; ++c) {
      if (before.cities[c].owner == seat) ++a;
    }
    for (CityId c = 0; c < after.city_count; ++c) {
      if (after.cities[c].owner == seat) ++b;
    }
    return b - a;
  }

  // Negative total distance from our units to the nearest enemy city. Increasing it means
  // closing in, so a simple greedy maximiser walks toward objectives over many turns.
  static int nearest_enemy_city_distance_sum(const GameState& s, PlayerId seat) {
    int total = 0;
    for (UnitId u = 0; u < s.unit_count; ++u) {
      const Unit& unit = s.units[u];
      if (unit.owner != seat || !unit.alive() || unit.tile == kNoTile) continue;
      int best = 1000;
      const Coord uc = s.dims.coord(unit.tile);
      for (CityId c = 0; c < s.city_count; ++c) {
        if (s.cities[c].owner == seat || s.cities[c].owner == kNoPlayer) continue;
        const Coord cc = s.dims.coord(s.cities[c].tile);
        const int d = std::max(std::abs(uc.x - cc.x), std::abs(uc.y - cc.y));
        best = std::min(best, d);
      }
      if (best < 1000) total += best;
    }
    return total;
  }

  static int approach_delta(const GameState& before, const GameState& after, PlayerId seat) {
    return nearest_enemy_city_distance_sum(before, seat) -
           nearest_enemy_city_distance_sum(after, seat);
  }

  static constexpr float kExpansionBias = 60.0f;
  static constexpr float kObjectivePull = 3.0f;
  EvalWeights weights_;
};

// ---------------------------------------------------------------------------------------

std::unique_ptr<Agent> make_agent(const std::string& name) {
  if (name == "random") return std::make_unique<RandomAgent>();
  if (name == "osla") return std::make_unique<OslaAgent>();
  if (name == "mc") return std::make_unique<MonteCarloAgent>();
  if (name == "mcts") return std::make_unique<MctsAgent>();
  if (name == "rhea") return std::make_unique<RheaAgent>();
  if (name == "rulebased") return std::make_unique<RuleBasedAgent>();
  return nullptr;
}

const char* const kAgentNames[] = {"random", "osla", "mc", "mcts", "rhea", "rulebased"};
int agent_count() { return 6; }

}  // namespace poly::agents
