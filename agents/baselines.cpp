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
      // STRICTLY greater. With >=, equal-valued candidates keep replacing the incumbent, and
      // the coarse evaluation ties constantly -- so the "hill climber" random-walked instead
      // of climbing. This alone made RHEA close to a random plan generator.
      if (value > best_value) {
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

    // Roll on to the TURN BOUNDARY before evaluating. A 12-decision horizon does not even
    // reach the end of the agent's own turn (turns run 20-40 decisions here), so the plan was
    // being scored on half-finished positions -- mid-purchase, mid-move, army stranded. That
    // is noise, not signal, and it is exactly what README §5 means by "search whole turns,
    // not individual actions".
    const PlayerId mover = working.current_player;
    for (int guard = 0; guard < kTurnRollout && working.current_player == mover; ++guard) {
      if (outcome(working, model.rules()).has_value()) break;
      ActionMask mask;
      legal_actions(working, model.rules(), mask);
      if (mask.empty()) break;
      if (!model.step(working, sample_uniform(mask, rng_), &mask)) break;
    }
    return evaluate_leaf(working, model.rules(), seat, weights_);
  }

  static constexpr int kHorizon = 12;
  static constexpr int kTurnRollout = 40;  // bound on finishing the turn
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
// Everything below the frozen baseline is behind a flag, and `rb_base` registers the agent
// with every flag OFF. That is not ceremony: HANDOFF §2 records two strong design instincts
// about this bot that measurement proved wrong (units-first selection cost 55 points of win
// rate), so the ability to A/B a single behaviour against a FROZEN reference -- rather than
// against a remembered number from an older binary -- is the thing that makes the advice
// "change one thing, measure, keep or revert" actually executable.
struct RuleBasedFlags {
  bool finish_turn{false};          // stop spinning on declined actors; issue a real EndTurn
  bool kill_not_wound{false};       // do not trade a wound the enemy heals back next turn
  bool resources_over_wall{false};  // level-3 reward: 5 stars unless actually threatened

  static RuleBasedFlags all() { return {true, true, true}; }
};

class RuleBasedAgent final : public Agent {
 public:
  explicit RuleBasedAgent(RuleBasedFlags flags, const char* name)
      : flags_(flags), name_(name) {}

  // The playable baseline. Earlier versions deliberately modelled the shipped bots' WEAKNESSES
  // (nearest-target attacking, no cost ceiling) to be a faithful clone. That is the wrong
  // objective for an opponent you are going to play against to check the game works, so the
  // handicaps are gone and this now tries to play well.
  //
  // It is scripted rather than searching, which is not a compromise here: at any budget we can
  // afford, the search agents lose to it. A domain policy that never wastes a decision beats a
  // generic search that spends most of its budget rediscovering that Deselect does nothing.
  Action choose(const GameState& s, ForwardModel& model, const ActionMask& mask,
                PlayerId seat) override {
    rules_ = model.rules();
    std::int16_t slots[kActionSlots];
    const int count = mask.collect(slots, kActionSlots);
    if (flags_.finish_turn) note_turn(s);

    if (s.phase == Phase::Verb) {
      // Capture is unconditionally first: a city is worth more than anything else a unit can
      // do with its turn, and expansion compounds.
      static const Verb kPriority[] = {
          Verb::Capture,  Verb::Examine,   Verb::ChooseReward, Verb::Attack,
          Verb::Harvest,  Verb::Build,     Verb::Train,        Verb::Move,
          Verb::Promote,  Verb::Recover,   Verb::Research,
      };
      for (const Verb verb : kPriority) {
        if (!mask.test(static_cast<int>(verb))) continue;
        // Only attack if a decent trade exists; otherwise fall through to moving, which keeps
        // the unit useful instead of feeding it to a defender.
        if (verb == Verb::Attack && !worthwhile_attack(s)) continue;
        // Only recover when actually hurt enough to matter.
        if (verb == Verb::Recover && !badly_hurt(s)) continue;
        return Action{static_cast<std::int16_t>(verb)};
      }
      // NOTHING THIS ACTOR IS WILLING TO DO. Falling straight through to Deselect made the
      // turn an unbounded spin, and MEASUREMENT is what found it: 88% of player-turns were
      // ending at the harness's 300-decision backstop rather than by choice, at ~269
      // decisions each.
      //
      // The cause is that the two verb gates above DECLINE a legal verb. An actor whose only
      // legal verbs are {Attack, Deselect} with no worthwhile attack -- or {Recover,
      // Deselect} while not badly hurt -- is answered with Deselect, and `legal_select` then
      // offers that same actor again, unchanged, forever. This is exactly the no-op cycle of
      // HANDOFF §3, and it was degrading the playable baseline itself.
      //
      // The fix is agent-side on purpose. Making Deselect illegal in the engine is an
      // ACTION-SPACE change, which WORKFLOW.md puts on the stop-and-ask list, and it would
      // also take "changed my mind" away from the human UI. A policy that declines an actor
      // simply has to remember that it declined.
      if (flags_.finish_turn) {
        if (!s.global_actor) mark_declined(s.selected);
        if (mask.test(static_cast<int>(Verb::EndTurn))) {
          return Action{static_cast<std::int16_t>(Verb::EndTurn)};
        }
      }
      if (mask.test(static_cast<int>(Verb::Deselect))) {
        return Action{static_cast<std::int16_t>(Verb::Deselect)};
      }
      return Action{slots[0]};
    }

    if (s.phase == Phase::Select) {
      // CITIES AND TILES BEFORE UNITS. I expected the opposite -- capture is the highest-value
      // verb and only units can do it -- and tried units-first on that reasoning. It cost 55
      // points of win rate against random (80% -> 25%). The economy actions are the ones that
      // compound, and spending stars before moving means a unit that dies this turn has
      // already been replaced. Measured, not reasoned.
      std::vector<std::int16_t> units, cities, tiles;
      for (int i = 0; i < count; ++i) {
        const int index = slots[i];
        if (index == kGlobalActorSlot) continue;
        const TileIdx tile = slot_to_tile(index);
        // An actor this policy already refused to use this turn is not offered again. Without
        // this the turn never terminates; see the Verb-phase comment above.
        if (flags_.finish_turn && declined(tile)) continue;
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
      if (!cities.empty()) return pick(cities);
      if (!tiles.empty()) return pick(tiles);
      if (!units.empty()) return pick(units);
      return Action{static_cast<std::int16_t>(kGlobalActorSlot)};
    }

    switch (s.pending_verb) {
      case Verb::Attack: return best_attack(s, slots, count);
      case Verb::Move: return best_move(s, slots, count, seat);
      case Verb::Train: return best_train(s, slots, count, seat);
      case Verb::Research: return best_research(s, slots, count, seat);
      case Verb::ChooseReward: return best_reward(s, slots, count);
      default: return sample_uniform(mask, rng_);
    }
  }
  const char* name() const override { return name_; }

  // The declined-actor set is the only state this agent carries between decisions, and it must
  // NOT carry between games. `seed()` is the per-game reset hook every agent already gets, so
  // clearing here is what makes "agents are stateless across games" (agent.hpp) true of this
  // one rather than merely intended.
  void seed(std::uint64_t value) override {
    Agent::seed(value);
    for (auto& word : declined_) word = 0;
    declined_turn_ = -1;
    declined_player_ = kNoPlayer;
  }

 private:
  RuleBasedFlags flags_;
  const char* name_;

  // --- declined-actor set ------------------------------------------------------------------
  // Cleared whenever the turn or the mover changes, so it is scoped to exactly one player-turn.
  // Keyed by tile because that is what a SELECT index is; the global actor is never recorded,
  // since declining it is what produces EndTurn.
  std::uint64_t declined_[(kMaxTiles + 63) / 64]{};
  std::int32_t declined_turn_{-1};
  PlayerId declined_player_{kNoPlayer};

  void note_turn(const GameState& s) {
    if (s.turn == declined_turn_ && s.current_player == declined_player_) return;
    declined_turn_ = s.turn;
    declined_player_ = s.current_player;
    for (auto& word : declined_) word = 0;
  }
  void mark_declined(TileIdx tile) {
    if (tile == kNoTile) return;
    declined_[tile >> 6] |= std::uint64_t{1} << (tile & 63);
  }
  [[nodiscard]] bool declined(TileIdx tile) const {
    return (declined_[tile >> 6] & (std::uint64_t{1} << (tile & 63))) != 0;
  }

  // --- combat judgement ---------------------------------------------------------------
  bool worthwhile_attack(const GameState& s) const {
    const Coord from = s.dims.coord(s.selected);
    const UnitId me = s.tiles[s.selected].unit;
    if (me == kNoUnit) return false;
    for (TileIdx t = 0; t < static_cast<TileIdx>(s.tile_count()); ++t) {
      const UnitId other = s.tiles[t].unit;
      if (other == kNoUnit || !s.units[other].alive()) continue;
      if (s.units[other].owner == s.units[me].owner) continue;
      const Coord tc = s.dims.coord(t);
      if (std::max(std::abs(from.x - tc.x), std::abs(from.y - tc.y)) >
          rules_.unit(s.units[me].type).range) {
        continue;
      }
      const CombatPreview p = preview_combat(s, rules_, s.selected, t);
      if (!p.valid) continue;
      if (p.defender_dies) return true;   // always take a kill
      if (p.attacker_dies) continue;      // never trade the unit away for a scratch

      // KILL, NOT WOUND -- the first entry in the community's catalogue of tactics, and the
      // wiki's "Attack to Kill" section: "It isn't always a good idea to attack, especially if
      // you leave your unit with half health so your opponent can kill it the next turn", and
      // "if you can only wound, make sure the wound takes more than 4 HP, because a wounded
      // unit in home area can be healed in a turn".
      //
      // Note what this is NOT. HANDOFF §2 records that gating attacks on a favourable damage
      // RATIO made the bot sit on its army and never close games out; that gate compared the
      // two sides' damage, so a well-defended target was never attackable at all. This gate is
      // absolute rather than comparative: it asks only whether the wound survives the
      // opponent's heal, so an attack that actually accomplishes something still goes in.
      if (flags_.kill_not_wound) {
        const CityId on_our_city = city_at(s, t);
        const bool clears_our_city =
            on_our_city != kNoCity && s.cities[on_our_city].owner == s.units[me].owner;
        // Recover restores 4 HP in own territory (game.cpp Verb::Recover), so a wound of 4 or
        // less is undone for free next turn and we paid retaliation for nothing.
        if (!clears_our_city && p.attacker_damage <= 4) continue;
      }
      return true;
    }
    return false;
  }

  Action best_attack(const GameState& s, const std::int16_t* slots, int count) const {
    Action best{slots[0]};
    int best_score = -(1 << 20);
    for (int i = 0; i < count; ++i) {
      const CombatPreview p = preview_combat(s, rules_, s.selected, slot_to_tile(slots[i]));
      if (!p.valid) continue;
      // Kills dominate; then net damage; dying is heavily penalised but not forbidden, so it
      // will still trade a wounded unit to finish something important.
      int score = p.attacker_damage - p.defender_damage;
      if (p.defender_dies) score += 200;
      if (p.attacker_dies) score -= 150;
      // Prefer clearing a unit that is sitting on one of our cities.
      const CityId city = city_at(s, slot_to_tile(slots[i]));
      if (city != kNoCity && s.cities[city].owner == s.current_player) score += 120;
      if (score > best_score) {
        best_score = score;
        best = Action{slots[i]};
      }
    }
    return best;
  }

  bool badly_hurt(const GameState& s) const {
    const UnitId me = s.tiles[s.selected].unit;
    if (me == kNoUnit) return false;
    const int max_hp = to_hp(rules_.unit(s.units[me].type).hp);
    return s.units[me].hp * 2 < max_hp;  // below half
  }

  // --- movement ------------------------------------------------------------------------
  Action best_move(const GameState& s, const std::int16_t* slots, int count,
                   PlayerId seat) const {
    Action best{slots[0]};
    int best_score = -(1 << 20);

    for (int i = 0; i < count; ++i) {
      const TileIdx t = slot_to_tile(slots[i]);
      const Coord tc = s.dims.coord(t);
      int score = 0;

      // Standing ON a village is what lets us capture next turn -- by far the strongest move
      // available, since expansion compounds and a village is a free city.
      if (s.tiles[t].resource == Resource::Village) score += 1000;
      if (s.tiles[t].resource == Resource::Ruin) score += 250;

      int nearest_village = 1 << 20, nearest_enemy_city = 1 << 20;
      for (TileIdx o = 0; o < static_cast<TileIdx>(s.tile_count()); ++o) {
        if (s.tiles[o].resource != Resource::Village) continue;
        const Coord oc = s.dims.coord(o);
        nearest_village = std::min(nearest_village,
                                   std::max(std::abs(tc.x - oc.x), std::abs(tc.y - oc.y)));
      }
      for (CityId c = 0; c < s.city_count; ++c) {
        if (s.cities[c].owner == seat || s.cities[c].owner == kNoPlayer) continue;
        const Coord cc = s.dims.coord(s.cities[c].tile);
        nearest_enemy_city = std::min(
            nearest_enemy_city, std::max(std::abs(tc.x - cc.x), std::abs(tc.y - cc.y)));
      }
      // Expansion outranks aggression, which is how the game actually rewards you.
      if (nearest_village < (1 << 20)) score -= nearest_village * 12;
      if (nearest_enemy_city < (1 << 20)) score -= nearest_enemy_city * 4;
      // A defence bonus is free value when it costs nothing to stand there -- but ONLY if we
      // have the tech that grants it. Forest needs Archery and Mountain needs Climbing
      // (game.cpp resolve_combat, and the wiki's Combat page agrees), so an un-teched bot was
      // paying position for a bonus it did not have.
      const Player& me = s.players[seat];
      const bool forest_bonus =
          s.tiles[t].terrain == Terrain::Forest && me.has_tech(Tech::Archery);
      const bool mountain_bonus =
          s.tiles[t].terrain == Terrain::Mountain && me.has_tech(Tech::Climbing);
      if (forest_bonus || mountain_bonus) score += 8;

      if (score > best_score) {
        best_score = score;
        best = Action{slots[i]};
      }
    }
    return best;
  }

  // --- production ----------------------------------------------------------------------
  Action best_train(const GameState& s, const std::int16_t* slots, int count,
                    PlayerId seat) const {
    // Buy the best unit affordable, not the cheapest. Cheapest floods the board with Warriors
    // and caps the army at the city's unit limit with the weakest bodies available.
    Action best{slots[0]};
    float best_value = -1e30f;
    for (int i = 0; i < count; ++i) {
      if (!slot_is_train(slots[i])) continue;
      const UnitType type = slot_to_unit_type(slots[i]);
      const UnitStats& u = rules_.unit(type);
      const float power = u.attack + u.defence + 0.4f * static_cast<float>(u.hp) / 10.0f +
                          0.5f * static_cast<float>(u.move) + 0.4f * static_cast<float>(u.range);
      // Slight preference for spending down, so it does not hoard while being invaded.
      const float value = power - 0.35f * static_cast<float>(u.cost);
      if (value > best_value) {
        best_value = value;
        best = Action{slots[i]};
      }
    }
    (void)s;
    (void)seat;
    return best;
  }

  Action best_research(const GameState& s, const std::int16_t* slots, int count,
                       PlayerId seat) const {
    // A curated opening rather than "cheapest". Cheapest walks the tree at random and never
    // reaches the techs that actually compound: economy first, then mobility, then force.
    static const Tech kOrder[] = {
        Tech::Organization, Tech::Hunting,   Tech::Fishing,  Tech::Riding,
        Tech::Climbing,     Tech::Archery,   Tech::Forestry, Tech::Farming,
        Tech::Strategy,     Tech::Mining,    Tech::Smithery, Tech::Mathematics,
        Tech::Roads,        Tech::Chivalry,  Tech::Trade,    Tech::FreeSpirit,
        Tech::Meditation,   Tech::Philosophy, Tech::Sailing, Tech::Ramming,
    };
    for (const Tech want : kOrder) {
      if (s.players[seat].has_tech(want)) continue;
      for (int i = 0; i < count; ++i) {
        if (slot_is_research(slots[i]) && slot_to_tech(slots[i]) == want) {
          return Action{slots[i]};
        }
      }
    }
    return Action{slots[0]};
  }

  Action best_reward(const GameState& s, const std::int16_t* slots, int count) const {
    // The engine offers rewards as LEVEL-GATED PAIRS (game.cpp legal_targets: L2
    // Workshop|Explorer, L3 CityWall|Resources, L4 PopGrowth|BorderGrowth, L5+ Park|SuperUnit),
    // which matches the wiki exactly. So only the order WITHIN each pair can ever matter, and
    // three of the four were already right:
    //
    //   Workshop  > Explorer   -- +1 income forever beats one reveal in a 2-player game
    //   PopGrowth > BorderGrowth -- 3 pop is worth ~6 stars; borders often add nothing
    //   SuperUnit > Park       -- this ladder plays Domination, where a Giant is the point
    //
    // The fourth was inverted. The competitive consensus is emphatic that the 5 stars beat the
    // wall: "Resources are almost always the better choice... City Walls are only useful when
    // your city is besieged" (wiki, Upgrading to Level 3), and the same page calls a level-3
    // upgrade with the 5-star rebate an effective cost of 1-2 stars, i.e. a 1-turn payback.
    // Taking the wall by default paid 5 stars every time for a bonus that does nothing unless
    // a specific, usually-false condition holds.
    //
    // The condition is kept rather than dropped, because the sources qualify it rather than
    // deny it: take the wall when this city is ACTUALLY under threat.
    const bool prefer_wall = flags_.resources_over_wall ? city_threatened(s, s.selected) : true;
    static const CityReward kWallFirst[] = {
        CityReward::Workshop,   CityReward::PopulationGrowth, CityReward::SuperUnit,
        CityReward::CityWall,   CityReward::Resources,        CityReward::BorderGrowth,
        CityReward::Park,       CityReward::Explorer,
    };
    static const CityReward kStarsFirst[] = {
        CityReward::Workshop,   CityReward::PopulationGrowth, CityReward::SuperUnit,
        CityReward::Resources,  CityReward::CityWall,         CityReward::BorderGrowth,
        CityReward::Park,       CityReward::Explorer,
    };
    const CityReward* const kOrder = prefer_wall ? kWallFirst : kStarsFirst;
    for (int k = 0; k < 8; ++k) {
      for (int i = 0; i < count; ++i) {
        if (slot_is_reward(slots[i]) && slot_to_reward(slots[i]) == kOrder[k]) {
          return Action{slots[i]};
        }
      }
    }
    return Action{slots[0]};
  }

  // --- threat model -------------------------------------------------------------------------
  // "Can an enemy body be on this tile, or hitting it, next turn?" Reach is move + range in
  // Chebyshev distance, which over-estimates (it ignores terrain and zone of control) and is
  // meant to: a defensive test that is wrong should be wrong in the direction of caution.
  [[nodiscard]] int enemy_reach(const GameState& s, TileIdx tile, PlayerId seat) const {
    const Coord tc = s.dims.coord(tile);
    int best = 1 << 20;
    for (UnitId u = 0; u < s.unit_count; ++u) {
      const Unit& e = s.units[u];
      if (!e.alive() || e.owner == seat || e.owner == kNoPlayer) continue;
      const Coord ec = s.dims.coord(e.tile);
      const int distance = std::max(std::abs(tc.x - ec.x), std::abs(tc.y - ec.y));
      const UnitStats& es = rules_.unit(e.type);
      best = std::min(best, distance - es.move - es.range);
    }
    return best;  // <= 0 means an enemy can be on or onto this tile next turn
  }

  // A city is threatened when an enemy can reach it next turn. Besieged counts outright.
  [[nodiscard]] bool city_threatened(const GameState& s, TileIdx city_tile) const {
    const CityId c = city_at(s, city_tile);
    if (c == kNoCity) return false;
    return enemy_reach(s, city_tile, s.cities[c].owner) <= 0;
  }

  Rules rules_;
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
  if (name == "rulebased") return std::make_unique<RuleBasedAgent>(RuleBasedFlags::all(),
                                                                   "rulebased");
  // The measurement reference: turn termination fixed, no strategy changes. Every strategy
  // flag on `rulebased` is A/B'd head to head against this rather than against a remembered
  // number from an older binary. Not part of the ladder.
  //
  // It is NOT the original policy, because that one never ended its own turn and so lost 88%
  // of its player-turns to the harness backstop -- measuring against it would credit every
  // later change with a share of that one fix.
  if (name == "rb_base") {
    RuleBasedFlags f;
    f.finish_turn = true;
    return std::make_unique<RuleBasedAgent>(f, "rb_base");
  }
  // rb_base plus exactly ONE strategy flag, so each can be measured on its own against
  // rb_base in a single build. HANDOFF §2: "Change one thing, measure, keep or revert. Do not
  // batch changes." These names exist to make that cheap, not to be shipped.
  static const struct {
    const char* name;
    RuleBasedFlags (*make)();
  } kProbes[] = {
      {"rb_kill", [] { RuleBasedFlags f; f.finish_turn = true; f.kill_not_wound = true; return f; }},
      {"rb_reward", [] { RuleBasedFlags f; f.finish_turn = true; f.resources_over_wall = true; return f; }},
  };
  for (const auto& probe : kProbes) {
    if (name == probe.name) {
      return std::make_unique<RuleBasedAgent>(probe.make(), probe.name);
    }
  }
  return nullptr;
}

const char* const kAgentNames[] = {"random", "osla", "mc", "mcts", "rhea", "paper2020",
                                   "rulebased"};
int agent_count() { return 7; }

}  // namespace poly::agents
