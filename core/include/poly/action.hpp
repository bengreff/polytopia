// The SELECT -> VERB -> TARGET phase machine and its action encoding.
//
// README §3 calls this "the single most important design decision in the problem", and
// docs/phase0-plan.md §3 builds it first. A Polytopia turn is a variable-length sequence of
// interdependent sub-actions; the 2020 paper measured branching PER TURN and got ~10^15,
// which scared the field off for six years. Decomposed into micro-decisions it is ~200 --
// smaller than Go.
//
// The encoding exploits one fact: SLOT MEANING IS REINTERPRETED PER PHASE. So the head is
// max(over phases) rather than the sum, and because units and cities both sit on tiles, a
// SELECT action index simply IS a tile index.
//
// For the 11x11 competitive format that gives 121 tile slots + 65 non-tile slots = 186.
// tribes-rl arrived independently at Discrete(197). Two designs landing within a dozen slots
// is the strongest evidence available that this decomposition is the natural one.
//
// The engine emits a legality mask per decision. Illegal actions are not discouraged; they
// are not representable. Legality is the engine's job, never the model's (README §4).

#pragma once

#include <cstdint>

#include "poly/content.hpp"
#include "poly/types.hpp"

namespace poly {

enum class Phase : std::uint8_t {
  Select = 0,  // Choose the actor. Index is a tile, or kGlobalActorSlot.
  Verb,        // Choose what it does. Index is a Verb.
  Target,      // Choose the target. Index is a tile or a typed slot; depends on the verb.
  Count,
};

enum class Verb : std::uint8_t {
  Move = 0,
  Attack,
  Capture,       // Requires having STARTED the turn on the tile (resolutions.md R25).
  Harvest,       // Fruit / Game / Fish / Starfish.
  Build,         // Place an Improvement.
  Train,         // Buy a unit in a city.
  UpgradeNaval,  // Raft -> Scout / Rammer / Bomber.
  Research,
  Heal,          // Mind Bender.
  Convert,       // Mind Bender.
  Examine,       // Enter a ruin. Same start-of-turn requirement as Capture.
  Destroy,       // Scorched earth: remove an improvement.
  Disband,
  ChooseReward,  // Resolve a pending city level-up choice.
  EndTurn,
  Deselect,      // Back out to SELECT without acting.
  Count,
};

inline constexpr int kVerbCount = static_cast<int>(Verb::Count);

// ---------------------------------------------------------------------------------------
// Action index layout
// ---------------------------------------------------------------------------------------
// Tile slots occupy [0, kMaxTiles) in every phase that addresses tiles. Typed targets are
// packed above them in fixed blocks, so decoding never depends on map size -- an action
// index means the same thing on an 11x11 and a 32x32 map, which keeps replays and trained
// policies portable across map sizes.

inline constexpr int kTileSlots = kMaxTiles;

inline constexpr int kTrainSlotBase = kTileSlots;
inline constexpr int kTrainSlotCount = kUnitTypeCount;

inline constexpr int kResearchSlotBase = kTrainSlotBase + kTrainSlotCount;
inline constexpr int kResearchSlotCount = kTechCount;

inline constexpr int kBuildSlotBase = kResearchSlotBase + kResearchSlotCount;
inline constexpr int kBuildSlotCount = static_cast<int>(Improvement::Count);

inline constexpr int kRewardSlotBase = kBuildSlotBase + kBuildSlotCount;
inline constexpr int kRewardSlotCount = static_cast<int>(CityReward::Count);

inline constexpr int kNonTileSlots =
    kTrainSlotCount + kResearchSlotCount + kBuildSlotCount + kRewardSlotCount;

// The full action head. Phases reinterpret the low slots, so this is a max, not a sum.
inline constexpr int kActionSlots = kRewardSlotBase + kRewardSlotCount;

// The global pseudo-actor: research and end-turn belong to the player, not to a unit or a
// city, and giving them a real slot avoids a separate "player action" code path.
inline constexpr int kGlobalActorSlot = kActionSlots - 1;

static_assert(kVerbCount <= kActionSlots, "The VERB phase reuses the same index space.");

// Head size for a given map. This is the number a policy network is built against.
constexpr int action_head_size(MapDims dims) {
  return dims.tile_count() + kNonTileSlots;
}

// ---------------------------------------------------------------------------------------
// Action
// ---------------------------------------------------------------------------------------
// One micro-decision. `index` is interpreted according to the state's current phase, which
// is why an Action alone is meaningless -- it is always applied to a state that knows what
// it was asked. That coupling is deliberate: it makes a stale or replayed action against a
// mismatched state a legality failure rather than a silent misinterpretation.
struct Action {
  std::int16_t index{0};

  friend constexpr bool operator==(Action, Action) = default;
};

// Decoders. Each asserts nothing -- callers use them only after checking the legality mask,
// which is what guarantees the slot class is right.
constexpr bool slot_is_tile(int index) { return index >= 0 && index < kTileSlots; }
constexpr TileIdx slot_to_tile(int index) { return static_cast<TileIdx>(index); }
constexpr int tile_to_slot(TileIdx tile) { return static_cast<int>(tile); }

constexpr bool slot_is_train(int index) {
  return index >= kTrainSlotBase && index < kTrainSlotBase + kTrainSlotCount;
}
constexpr UnitType slot_to_unit_type(int index) {
  return static_cast<UnitType>(index - kTrainSlotBase);
}
constexpr int unit_type_to_slot(UnitType t) {
  return kTrainSlotBase + static_cast<int>(t);
}

constexpr bool slot_is_research(int index) {
  return index >= kResearchSlotBase && index < kResearchSlotBase + kResearchSlotCount;
}
constexpr Tech slot_to_tech(int index) { return static_cast<Tech>(index - kResearchSlotBase); }
constexpr int tech_to_slot(Tech t) { return kResearchSlotBase + static_cast<int>(t); }

constexpr bool slot_is_build(int index) {
  return index >= kBuildSlotBase && index < kBuildSlotBase + kBuildSlotCount;
}
constexpr Improvement slot_to_improvement(int index) {
  return static_cast<Improvement>(index - kBuildSlotBase);
}
constexpr int improvement_to_slot(Improvement i) {
  return kBuildSlotBase + static_cast<int>(i);
}

constexpr bool slot_is_reward(int index) {
  return index >= kRewardSlotBase && index < kRewardSlotBase + kRewardSlotCount;
}
constexpr CityReward slot_to_reward(int index) {
  return static_cast<CityReward>(index - kRewardSlotBase);
}
constexpr int reward_to_slot(CityReward r) { return kRewardSlotBase + static_cast<int>(r); }

// ---------------------------------------------------------------------------------------
// Legality mask
// ---------------------------------------------------------------------------------------
// A fixed-size bitset over the action head. Returned by the engine each decision; the only
// sanctioned way to discover what may be done.
class ActionMask {
 public:
  static constexpr int kWords = (kActionSlots + 63) / 64;

  constexpr void clear() {
    for (auto& word : words_) word = 0;
    count_ = 0;
  }

  constexpr void set(int index) {
    if (!test(index)) ++count_;
    words_[index >> 6] |= (std::uint64_t{1} << (index & 63));
  }

  [[nodiscard]] constexpr bool test(int index) const {
    return (words_[index >> 6] & (std::uint64_t{1} << (index & 63))) != 0;
  }

  [[nodiscard]] constexpr int count() const { return count_; }
  [[nodiscard]] constexpr bool empty() const { return count_ == 0; }

  // Enumerate legal indices into `out`, returning how many were written. Used by the UI to
  // populate menus and by search to expand a node.
  int collect(std::int16_t* out, int capacity) const {
    int written = 0;
    for (int word = 0; word < kWords && written < capacity; ++word) {
      std::uint64_t bits = words_[word];
      while (bits != 0 && written < capacity) {
        const int bit = __builtin_ctzll(bits);
        bits &= bits - 1;
        out[written++] = static_cast<std::int16_t>((word << 6) | bit);
      }
    }
    return written;
  }

  friend constexpr bool operator==(const ActionMask&, const ActionMask&) = default;

 private:
  std::uint64_t words_[kWords]{};
  std::int32_t count_{0};
};

}  // namespace poly
