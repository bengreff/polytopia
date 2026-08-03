// GameState -- the complete, trivially-copyable game position.
//
// HARD CONSTRAINT: no heap, no pointers, no std::vector, no virtuals. GameState must be
// memcpy-cloneable, because a forward model that can be copied cheaply is the difference
// between a search that works and one that does not (docs/phase0-plan.md §3).
//
// Rules parameters are deliberately NOT here. `Rules` is passed as a const reference to
// every function, so one shared instance serves thousands of parallel environments and
// clones stay small.

#pragma once

#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

#include "poly/action.hpp"
#include "poly/content.hpp"
#include "poly/rng.hpp"
#include "poly/types.hpp"

namespace poly {

// HP is stored in HALF-POINT units: hp = 2 means 1.0 HP (docs/rules-research/resolutions.md
// R13). UNRESOLVED #1 is a direct contradiction between primary sources on whether splash
// damage rounds, and the Combat page documents fractional HP as an observable display bug.
// Scaling by 2 represents .5 exactly, keeps the state bit-hashable for the determinism test,
// and makes either reading a one-line change at one call site.
inline constexpr std::int16_t kHpScale = 2;

constexpr std::int16_t to_hp(int whole) { return static_cast<std::int16_t>(whole * kHpScale); }
constexpr int hp_whole(std::int16_t hp) { return hp / kHpScale; }

// ---------------------------------------------------------------------------------------

enum class TileFlag : std::uint8_t {
  None = 0,
  Road = 1u << 0,
  Bridge = 1u << 1,  // A road over shallow water.
};

struct Tile {
  Terrain terrain{Terrain::Field};
  Resource resource{Resource::None};
  Improvement improvement{Improvement::None};

  // Territory owner. Borders are an ARBITRARY TILE SET, not a rectangle: Border Growth never
  // claims already-owned tiles, so real borders are ragged (resolutions.md R3). Storing an
  // owner per tile is the only representation that can express that; a per-city rectangle
  // would compile, pass tests, and be wrong in most real games.
  PlayerId owner{kNoPlayer};
  CityId city{kNoCity};  // Which city claims this tile.

  UnitId unit{kNoUnit};  // Occupant, if any.
  std::uint8_t flags{0};

  [[nodiscard]] constexpr bool has(TileFlag f) const {
    return (flags & static_cast<std::uint8_t>(f)) != 0;
  }
  constexpr void add(TileFlag f) { flags |= static_cast<std::uint8_t>(f); }

  friend constexpr bool operator==(const Tile&, const Tile&) = default;
};

// ---------------------------------------------------------------------------------------

enum class UnitFlag : std::uint16_t {
  None = 0,
  HasMoved = 1u << 0,
  HasAttacked = 1u << 1,
  TurnEnded = 1u << 2,   // Set by any turn-ending action (resolutions.md R26).
  Veteran = 1u << 3,
  Embarked = 1u << 4,    // Carried by a naval hull; see `carried_type`.
  FreshlyTrained = 1u << 5,  // Cannot act until next turn (R2).
  ArrivedThisTurn = 1u << 6,  // Blocks Capture and Examine, which need a turn start here (R25).
};

struct Unit {
  UnitType type{UnitType::Warrior};
  PlayerId owner{kNoPlayer};
  TileIdx tile{kNoTile};

  // Half-point units. A naval hull carries NO HP of its own -- an embarked unit is the SAME
  // entity, and damage carries both ways through embark/upgrade/disembark (R29). So this
  // field is the land unit's HP throughout, and `carried_type` remembers what to restore.
  std::int16_t hp{0};
  UnitType carried_type{UnitType::Warrior};

  CityId home_city{kNoCity};
  std::uint8_t moves_left{0};
  std::uint16_t flags{0};

  [[nodiscard]] constexpr bool has(UnitFlag f) const {
    return (flags & static_cast<std::uint16_t>(f)) != 0;
  }
  constexpr void add(UnitFlag f) { flags |= static_cast<std::uint16_t>(f); }
  constexpr void remove(UnitFlag f) { flags &= static_cast<std::uint16_t>(~static_cast<std::uint16_t>(f)); }

  [[nodiscard]] constexpr bool alive() const { return hp > 0; }

  friend constexpr bool operator==(const Unit&, const Unit&) = default;
};

// ---------------------------------------------------------------------------------------

enum class CityFlag : std::uint16_t {
  None = 0,
  Capital = 1u << 0,
  Walled = 1u << 1,
  Workshop = 1u << 2,
  BorderGrown = 1u << 3,
  ConnectedToCapital = 1u << 4,
  Besieged = 1u << 5,  // An enemy unit stands on it. Produces 0 income (R4).
};

struct City {
  PlayerId owner{kNoPlayer};
  TileIdx tile{kNoTile};

  // MONOTONIC. Never recomputed from population. Capturing a city re-parents it into the
  // captor's trade network and can drive population deeply negative, but the city does NOT
  // drop a level (resolutions.md R17). Deriving level from population -- the natural
  // implementation -- would silently demote every captured city.
  std::int8_t level{1};

  // SIGNED, and genuinely goes negative. An unsigned counter would underflow to a huge
  // positive on the exact turn a capital is captured, which is the single most important
  // moment of a Might game. Each negative point also costs 1 star/turn of income.
  std::int16_t population{0};

  std::uint8_t park_count{0};  // Repeatable L5+ reward; each gives +1 star/turn.
  std::uint8_t unit_count{0};  // Against capacity = level + 1.
  std::uint16_t flags{0};

  // A level-up the player has not yet resolved. While set, the only legal verb is
  // ChooseReward -- the phase machine will not let play continue around a pending choice.
  std::int8_t pending_rewards{0};

  [[nodiscard]] constexpr bool has(CityFlag f) const {
    return (flags & static_cast<std::uint16_t>(f)) != 0;
  }
  constexpr void add(CityFlag f) { flags |= static_cast<std::uint16_t>(f); }
  constexpr void remove(CityFlag f) { flags &= static_cast<std::uint16_t>(~static_cast<std::uint16_t>(f)); }

  [[nodiscard]] constexpr int unit_capacity() const { return level + 1; }

  friend constexpr bool operator==(const City&, const City&) = default;
};

// ---------------------------------------------------------------------------------------

struct Player {
  Tribe tribe{Tribe::XinXi};
  Difficulty difficulty{Difficulty::Normal};
  bool is_human{false};
  bool alive{true};

  std::int32_t stars{0};
  std::int32_t score{0};

  // 25 techs fit in a 32-bit mask.
  std::uint32_t techs{0};

  // Who this player has ENCOUNTERED, as a bitmask of PlayerId. Meeting a tribe is an event,
  // not a passive flag: first sight of their unit or city reveals their identity and pays
  // stars scaled to their score (resolutions.md C3). Until met, an opponent's tribe is
  // unknown -- and their stars, income and techs are never visible at all.
  std::uint16_t met{0};

  [[nodiscard]] constexpr bool has_met(PlayerId other) const {
    return (met & (1u << other)) != 0;
  }
  constexpr void mark_met(PlayerId other) {
    met = static_cast<std::uint16_t>(met | (1u << other));
  }

  [[nodiscard]] constexpr bool has_tech(Tech t) const {
    return (techs & (1u << static_cast<int>(t))) != 0;
  }
  constexpr void grant_tech(Tech t) { techs |= (1u << static_cast<int>(t)); }
  [[nodiscard]] constexpr int tech_count() const { return __builtin_popcount(techs); }

  friend constexpr bool operator==(const Player&, const Player&) = default;
};

static_assert(kTechCount <= 32, "Player::techs is a 32-bit mask.");

// ---------------------------------------------------------------------------------------

// Fog. ONE bitset per player, not the usual explored + currently-visible pair.
//
// Revealed tiles never re-fog and continue to show live events (resolutions.md R18), so
// there is no remembered stale state to model. Imperfect information in this game is
// exclusively about UNEXPLORED regions. That is a materially easier belief-state problem
// than README §5 Phase 5 assumes.
struct ExploredMap {
  static constexpr int kWords = (kMaxTiles + 63) / 64;
  std::uint64_t words[kWords]{};

  [[nodiscard]] constexpr bool test(TileIdx t) const {
    return (words[t >> 6] & (std::uint64_t{1} << (t & 63))) != 0;
  }
  constexpr void set(TileIdx t) { words[t >> 6] |= (std::uint64_t{1} << (t & 63)); }

  friend constexpr bool operator==(const ExploredMap&, const ExploredMap&) = default;
};

// ---------------------------------------------------------------------------------------

struct GameState {
  MapDims dims{};
  std::int32_t turn{0};

  // Whose decision this is. NEVER inferred from search depth. Polytopia allows consecutive
  // same-player moves, and inferring perspective from depth parity is exactly the bug that
  // crippled Polyfish's search (docs/research-findings.md §4).
  PlayerId current_player{0};

  std::int8_t player_count{0};
  std::int16_t unit_count{0};
  std::int16_t city_count{0};

  // Victory conditions travel with the state rather than living in the engine, so "any game
  // mode" is a configuration rather than a code change (docs/phase0-plan.md §4). A search
  // that clones a position must carry them too, or a cloned node would score differently
  // from its parent.
  GameMode mode{GameMode::Domination};
  std::int32_t turn_limit{0};  // 0 = none
  bool fog{true};

  // Phase-machine cursor.
  Phase phase{Phase::Select};
  TileIdx selected{kNoTile};    // kNoTile with phase != Select means the global pseudo-actor.
  Verb pending_verb{Verb::Move};
  bool global_actor{false};

  Pcg32 rng{};

  Tile tiles[kMaxTiles]{};
  Unit units[kMaxUnits]{};
  City cities[kMaxCities]{};
  Player players[kMaxPlayers]{};
  ExploredMap explored[kMaxPlayers]{};

  [[nodiscard]] constexpr const Tile& tile(TileIdx t) const { return tiles[t]; }
  [[nodiscard]] constexpr Tile& tile(TileIdx t) { return tiles[t]; }
  [[nodiscard]] constexpr const Player& me() const { return players[current_player]; }
  [[nodiscard]] constexpr Player& me() { return players[current_player]; }

  [[nodiscard]] constexpr int tile_count() const { return dims.tile_count(); }
};

// Canonically zeroes a GameState, PADDING INCLUDED.
//
// `state = GameState{}` is not enough. Value-initialisation sets every declared member but
// leaves padding bytes indeterminate -- Tile declares 9 bytes of members in a 10-byte struct,
// and that tenth byte keeps whatever was in the allocation. Two states built from identical
// inputs then compare equal member-wise but hash differently under memcmp or a byte hash.
//
// This was found the hard way: map generation's determinism test failed against a correct
// algorithm. Anything that byte-hashes or memcmps a GameState -- replay verification,
// transposition tables, the golden-trace harness -- depends on this function having been
// used. Reach for reset() rather than `= GameState{}` everywhere.
inline void reset(GameState& s) {
  std::memset(static_cast<void*>(&s), 0, sizeof(GameState));
  new (&s) GameState();
}

// FNV-1a over the whole state. Valid only on a state that came from reset(); see above.
inline std::uint64_t state_hash(const GameState& s) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(&s);
  std::uint64_t hash = 1469598103934665603ULL;
  for (std::size_t i = 0; i < sizeof(GameState); ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

// The constraint the whole design rests on. If this ever fails, someone added a std::vector
// or a pointer to the state and the forward model just became uncloneable.
static_assert(std::is_trivially_copyable_v<GameState>,
              "GameState must be memcpy-cloneable. No heap, no pointers, no virtuals.");
static_assert(std::is_trivially_copyable_v<Tile>);
static_assert(std::is_trivially_copyable_v<Unit>);
static_assert(std::is_trivially_copyable_v<City>);
static_assert(std::is_trivially_copyable_v<Player>);

// Budget guard. Not a correctness requirement, but a regression here means every search node
// got more expensive, and that is worth failing the build over rather than discovering later.
static_assert(sizeof(GameState) <= 64 * 1024, "GameState exceeded its 64KB budget.");

}  // namespace poly
