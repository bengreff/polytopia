// Game termination and per-player value.
//
// This header exists to make two documented failure modes structurally unrepresentable.
// Both come from docs/research-findings.md §4, the expert review of Polyfish — the only
// project in this space that measured itself, whose search was crippled by exactly these.
//
// FAILURE 1: "Terminal states and horizon cutoffs both return 0.0, so capturing the enemy
// capital looks identical to a draw inside the search."
//
//   Defence: termination is a type, not a float. game::outcome() returns
//   std::optional<Outcome>. A finished game yields an Outcome carrying how it ended and
//   who won. A horizon cutoff is the ABSENCE of an Outcome — it is std::nullopt, the same
//   as any other ongoing position, because that is exactly what it is. A searcher that
//   stops early must substitute its own heuristic estimate; there is no code path by which
//   the engine hands it a number that could be mistaken for a real result.
//
// FAILURE 2: "Value backprop flips sign at every tree edge by depth parity, but Polytopia
// allows consecutive same-player moves. Result: nodes frequently prefer children whose
// evaluations were bad for it."
//
//   Defence: nothing here is scalar. Every value is a PlayerValues array indexed by
//   PlayerId. There is no signature in this API that returns "the value" of a position,
//   so there is no way to write code that infers whose value it is from tree depth. The
//   caller must name the player. This costs kMaxPlayers * 4 bytes and removes an entire
//   bug class.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <type_traits>

#include "poly/types.hpp"

namespace poly {

// How a game ended. Note there is deliberately no `Ongoing` member: an ongoing game does
// not have an Outcome at all. Adding `Ongoing` here would reintroduce the ability to hold
// a "result" for an unfinished game, which is the first half of Failure 1.
enum class Termination : std::uint8_t {
  // Mode victory condition met — capital domination in Might, or a decisive lead.
  Victory = 0,
  // Turn limit reached with a winner decided on score. This is a REAL result, distinct
  // from a search horizon cutoff, which never produces an Outcome.
  ScoreAtTurnLimit,
  // Every remaining player but one has been eliminated.
  LastStanding,
  // Explicit resignation or disconnect.
  Resignation,
  Count,
};

// Per-player scalars. Used for scores, evaluations, and win probabilities alike.
// Always indexed by PlayerId; never reduced to a single number by the engine.
using PlayerValues = std::array<std::int32_t, kMaxPlayers>;

struct Outcome {
  Termination how{Termination::Victory};

  // kNoPlayer means a genuine draw. Draws are rare but representable; a draw is not the
  // same as an unevaluated position, which is the whole point of this header.
  PlayerId winner{kNoPlayer};

  // Final score for every player, for score-mode ranking and for training targets.
  PlayerValues score{};

  // Turn on which the game ended. Feeds the speed bonus in domination reward shaping.
  std::int32_t turn{0};

  [[nodiscard]] constexpr bool is_draw() const { return winner == kNoPlayer; }

  friend constexpr bool operator==(const Outcome&, const Outcome&) = default;
};

// The engine's termination query returns this. std::nullopt means the game is still going.
using MaybeOutcome = std::optional<Outcome>;

static_assert(std::is_trivially_copyable_v<Outcome>);

}  // namespace poly
