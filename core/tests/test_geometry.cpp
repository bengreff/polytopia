#include "poly/outcome.hpp"
#include "poly/types.hpp"
#include "testing.hpp"

using poly::Coord;
using poly::MapDims;

TEST("geometry: index and coord round-trip over a non-square map") {
  // Non-square on purpose. A width/height swap in row-major indexing is invisible on
  // square maps, and 11x11 is the format this project spends all its time on.
  const MapDims dims{7, 5};
  CHECK_EQ(dims.tile_count(), 35);

  for (std::int16_t y = 0; y < dims.height; ++y) {
    for (std::int16_t x = 0; x < dims.width; ++x) {
      const Coord c{x, y};
      CHECK(dims.coord(dims.index(c)) == c);
    }
  }
}

TEST("geometry: index is row-major") {
  const MapDims dims{7, 5};
  CHECK_EQ(dims.index(Coord{0, 0}), 0);
  CHECK_EQ(dims.index(Coord{1, 0}), 1);
  CHECK_EQ(dims.index(Coord{0, 1}), 7);
  CHECK_EQ(dims.index(Coord{6, 4}), 34);
}

TEST("geometry: contains rejects every out-of-bounds direction") {
  const MapDims dims{7, 5};
  CHECK(dims.contains(Coord{0, 0}));
  CHECK(dims.contains(Coord{6, 4}));
  CHECK(!dims.contains(Coord{-1, 0}));
  CHECK(!dims.contains(Coord{0, -1}));
  CHECK(!dims.contains(Coord{7, 0}));
  CHECK(!dims.contains(Coord{0, 5}));
}

TEST("geometry: adjacency is 8-way") {
  // Polytopia is 8-way: units move diagonally, and the "adjacent" that building bonuses
  // and border growth are defined in terms of includes diagonals. A 4-way slip here would
  // be silent and would corrupt every adjacency-scored building in the game.
  CHECK_EQ(poly::kNeighbourCount, 8);

  const MapDims dims{5, 5};
  const Coord centre{2, 2};
  int in_bounds = 0;
  for (const Coord offset : poly::kNeighbourOffsets) {
    const Coord n{static_cast<std::int16_t>(centre.x + offset.x),
                  static_cast<std::int16_t>(centre.y + offset.y)};
    CHECK(!(n == centre));
    if (dims.contains(n)) ++in_bounds;
  }
  CHECK_EQ(in_bounds, 8);

  // A corner sees exactly three neighbours.
  const Coord corner{0, 0};
  int corner_neighbours = 0;
  for (const Coord offset : poly::kNeighbourOffsets) {
    const Coord n{static_cast<std::int16_t>(corner.x + offset.x),
                  static_cast<std::int16_t>(corner.y + offset.y)};
    if (dims.contains(n)) ++corner_neighbours;
  }
  CHECK_EQ(corner_neighbours, 3);
}

TEST("geometry: the largest supported map fits the tile capacity") {
  const MapDims dims{poly::kMaxMapDim, poly::kMaxMapDim};
  CHECK_EQ(dims.tile_count(), poly::kMaxTiles);
  CHECK(dims.index(Coord{poly::kMaxMapDim - 1, poly::kMaxMapDim - 1}) < poly::kMaxTiles);
}

TEST("outcome: an ongoing game has no Outcome at all") {
  // The structural defence from docs/phase0-plan.md §1.1. A search horizon cutoff is
  // nullopt, exactly like any other unfinished position, so it can never be confused with
  // a real terminal result. Polyfish returned 0.0 for both and could not tell a captured
  // capital from a draw.
  poly::MaybeOutcome ongoing;
  CHECK(!ongoing.has_value());

  poly::Outcome decided;
  decided.how = poly::Termination::Victory;
  decided.winner = 1;
  decided.turn = 42;
  const poly::MaybeOutcome finished = decided;
  CHECK(finished.has_value());
  CHECK(!finished->is_draw());
  CHECK_EQ(finished->winner, 1);
}

TEST("outcome: a draw is distinct from an unfinished game") {
  poly::Outcome drawn;
  drawn.how = poly::Termination::ScoreAtTurnLimit;
  drawn.winner = poly::kNoPlayer;
  const poly::MaybeOutcome finished = drawn;
  CHECK(finished.has_value());
  CHECK(finished->is_draw());
}

TEST("outcome: value is per-player, never a bare scalar") {
  // The second structural defence. Nothing in the API returns "the value" of a position,
  // so no caller can infer whose value it is from tree depth parity -- the bug that made
  // Polyfish's search prefer children that were bad for the player to move.
  poly::PlayerValues values{};
  values[0] = 100;
  values[1] = -100;
  CHECK_EQ(values.size(), static_cast<std::size_t>(poly::kMaxPlayers));
  CHECK_EQ(values[0], 100);
  CHECK_EQ(values[1], -100);
}
