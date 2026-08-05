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
    if (!model.step(s, sample_uniform(mask, rng), &mask)) break;  // budget gone
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
      // `mask` was computed for `s`, and `next` is a clone of it, so validation need not be
      // repeated -- that was about half the cost of a node.
      if (!model.step(next, Action{slots[i]}, &mask)) break;
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
        if (!model.step(next, Action{slots[i]}, &mask)) break;
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
      if (!model.step(working, action, &mask)) break;
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
      if (!model.step(next, Action{slots[i]}, &mask)) break;
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
// The 2020 paper's rule-based agent -- a SEPARATE agent from the in-game bot clone above.
// ---------------------------------------------------------------------------------------
// These were conflated in the first pass, which made the Phase 1 gate unevaluable: the gate
// requires reproducing the PAPER's baseline (published 2nd of 6 at 65.80%), while `rulebased`
// clones the SHIPPED bots, which by the owner's own description fight badly. One is a
// benchmark anchor, the other is a sparring partner. Both are needed; they are not the same
// agent and must not share a name.
//
// The paper describes a heuristic agent with no search, so this is exactly the handcrafted
// evaluation applied greedily -- no expansion bias, no objective pull, no bot mimicry.
class Paper2020Agent final : public Agent {
 public:
  // A SCRIPTED PRIORITY POLICY. The paper's agent is "rule-based heuristic (NO SEARCH)", so
  // it must not consult the forward model at all -- an agent that evaluates successors is
  // one-step lookahead, which is a different published baseline (OSLA, 47.76%) that this
  // would otherwise duplicate under a second name.
  Action choose(const GameState& s, ForwardModel& model, const ActionMask& mask,
                PlayerId seat) override {
    // The rules are read for stats and previews. The forward MODEL is never stepped -- this
    // agent has no search, which is what distinguishes it from OSLA.
    model_rules_ = model.rules();
    std::int16_t slots[kActionSlots];
    const int count = mask.collect(slots, kActionSlots);

    // In the VERB phase the index IS the verb, so priority is a direct lookup.
    if (s.phase == Phase::Verb) {
      static const Verb kPriority[] = {
          Verb::Capture,   // free cities are the dominant source of everything
          Verb::Examine,   // ruins are free value
          Verb::Harvest,   // growth before force
          Verb::Attack,
          Verb::Build,
          Verb::Train,
          Verb::Promote,
          Verb::Recover,
          Verb::Move,
          Verb::Research,
          Verb::ChooseReward,
      };
      for (const Verb verb : kPriority) {
        if (mask.test(static_cast<int>(verb))) return Action{static_cast<std::int16_t>(verb)};
      }
      // Nothing worth doing with this actor.
      if (mask.test(static_cast<int>(Verb::Deselect))) {
        return Action{static_cast<std::int16_t>(Verb::Deselect)};
      }
      if (mask.test(static_cast<int>(Verb::EndTurn))) {
        return Action{static_cast<std::int16_t>(Verb::EndTurn)};
      }
      return Action{slots[0]};
    }

    // SELECT: prefer an actor that can do something valuable. Cities first (production is
    // the engine of the game), then units, and the global actor last so research and
    // end-turn only happen once the board is played out.
    if (s.phase == Phase::Select) {
      // Candidates are collected by class and then chosen RANDOMLY within the best class.
      // Picking deterministically loops: the agent selects the same city, finds nothing it
      // wants, deselects, and selects it again -- burning the whole turn against the
      // decision cap. That made this agent LOSE to random, 16.7%. The no-op cycle the
      // phase machine allows punishes deterministic policies specifically.
      std::vector<std::int16_t> cities, units, tiles;
      for (int i = 0; i < count; ++i) {
        const int index = slots[i];
        if (index == kGlobalActorSlot) continue;
        const TileIdx tile = slot_to_tile(index);
        const CityId city = city_at(s, tile);
        if (city != kNoCity && s.cities[city].owner == seat) {
          cities.push_back(static_cast<std::int16_t>(index));
        } else if (s.tiles[tile].unit != kNoUnit) {
          units.push_back(static_cast<std::int16_t>(index));
        } else {
          tiles.push_back(static_cast<std::int16_t>(index));
        }
      }
      const auto pick = [&](std::vector<std::int16_t>& v) {
        return Action{v[rng_.bounded(static_cast<std::uint32_t>(v.size()))]};
      };
      if (!units.empty()) return pick(units);    // units first: they capture and fight
      if (!cities.empty()) return pick(cities);
      if (!tiles.empty()) return pick(tiles);
      return Action{static_cast<std::int16_t>(kGlobalActorSlot)};
    }

    // TARGET. Uniform choice here is what kept this agent at 50% against random: picking
    // the verb well and then the target blindly throws away almost all of the policy.
    switch (s.pending_verb) {
      case Verb::Attack: {
        // Prefer a kill; otherwise the most damage. This is the whole of the agent's
        // tactical judgement and it costs one combat preview per candidate.
        Action best{slots[0]};
        int best_score = -1;
        for (int i = 0; i < count; ++i) {
          const CombatPreview p =
              preview_combat(s, model_rules_, s.selected, slot_to_tile(slots[i]));
          if (!p.valid) continue;
          int score = p.attacker_damage;
          if (p.defender_dies) score += 100;
          if (p.attacker_dies) score -= 200;  // never trade the unit away for nothing
          if (score > best_score) {
            best_score = score;
            best = Action{slots[i]};
          }
        }
        return best;
      }
      case Verb::Move: {
        // Close on the nearest enemy city or unclaimed village. Expansion and pressure are
        // the two things the paper's evaluation rewards most heavily.
        Action best{slots[0]};
        int best_distance = 1 << 20;
        for (int i = 0; i < count; ++i) {
          const TileIdx t = slot_to_tile(slots[i]);
          const Coord tc = s.dims.coord(t);
          int nearest = 1 << 20;
          if (s.tiles[t].resource == Resource::Village) nearest = 0;
          for (CityId c = 0; c < s.city_count; ++c) {
            if (s.cities[c].owner == seat || s.cities[c].owner == kNoPlayer) continue;
            const Coord cc = s.dims.coord(s.cities[c].tile);
            nearest = std::min(nearest,
                               std::max(std::abs(tc.x - cc.x), std::abs(tc.y - cc.y)));
          }
          if (nearest < best_distance) {
            best_distance = nearest;
            best = Action{slots[i]};
          }
        }
        return best;
      }
      case Verb::Train: {
        // Cheapest affordable unit -- bodies win the early game, and the evaluation counts
        // units linearly.
        Action best{slots[0]};
        int cheapest = 1 << 20;
        for (int i = 0; i < count; ++i) {
          if (!slot_is_train(slots[i])) continue;
          const int cost = model_rules_.unit(slot_to_unit_type(slots[i])).cost;
          if (cost < cheapest) {
            cheapest = cost;
            best = Action{slots[i]};
          }
        }
        return best;
      }
      case Verb::Research: {
        Action best{slots[0]};
        int cheapest = 1 << 20;
        int cities = 0;
        for (CityId c = 0; c < s.city_count; ++c) {
          if (s.cities[c].owner == seat) ++cities;
        }
        for (int i = 0; i < count; ++i) {
          if (!slot_is_research(slots[i])) continue;
          const int cost = model_rules_.tech_cost(slot_to_tech(slots[i]), cities,
                                                  s.players[seat].has_tech(Tech::Philosophy));
          if (cost < cheapest) {
            cheapest = cost;
            best = Action{slots[i]};
          }
        }
        return best;
      }
      default: return sample_uniform(mask, rng_);
    }
  }
  const char* name() const override { return "paper2020"; }

 private:
  Rules model_rules_;
};

// ---------------------------------------------------------------------------------------

std::unique_ptr<Agent> make_agent(const std::string& name) {
  if (name == "paper2020") return std::make_unique<Paper2020Agent>();
  if (name == "random") return std::make_unique<RandomAgent>();
  if (name == "osla") return std::make_unique<OslaAgent>();
  if (name == "mc") return std::make_unique<MonteCarloAgent>();
  if (name == "mcts") return std::make_unique<MctsAgent>();
  if (name == "rhea") return std::make_unique<RheaAgent>();
  if (name == "rulebased") return std::make_unique<RuleBasedAgent>();
  return nullptr;
}

const char* const kAgentNames[] = {"random", "osla", "mc", "mcts", "rhea", "paper2020",
                                   "rulebased"};
int agent_count() { return 7; }

}  // namespace poly::agents
