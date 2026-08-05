// Core index types, capacity limits, and map geometry.
//
// Nothing in this header knows a game rule. Content enums (units, techs, buildings)
// arrive in M2, sourced from docs/rules-research/live-rules.md. Capacities live here
// because they determine sizeof(GameState), which the whole design depends on.

#pragma once

#include <cstdint>

namespace poly {

// Index types are signed and narrow. Signed so that -1 is a natural "none" without a
// separate optional wrapper; narrow because every one of these appears in arrays sized
// by the capacities below, and sizeof(GameState) is a first-class design constraint.
using PlayerId = std::int8_t;
using TileIdx = std::int16_t;
using UnitId = std::int16_t;
using CityId = std::int16_t;

inline constexpr PlayerId kNoPlayer = -1;
inline constexpr TileIdx kNoTile = -1;
inline constexpr UnitId kNoUnit = -1;
inline constexpr CityId kNoCity = -1;

// Capacities. These are compile-time so that GameState is a fixed-size POD; see state.hpp.
//
// kMaxMapDim of 32 covers every real-game map size with headroom. The competitive format
// this project targets is 11x11 (121 tiles), so the common case uses ~12% of the tile
// array. GameState::clone copies only the live prefix rather than the full capacity.
inline constexpr int kMaxMapDim = 32;
inline constexpr int kMaxTiles = kMaxMapDim * kMaxMapDim;  // 1024
inline constexpr int kMaxPlayers = 16;
inline constexpr int kMaxUnits = 512;
inline constexpr int kMaxCities = 128;

// Terrain. This is the one content enum settled early enough to belong in M0: it is
// invariant across every source consulted, and map geometry cannot be expressed without it.
enum class Terrain : std::uint8_t {
  Field = 0,
  Forest,
  Mountain,
  Water,   // shallow; adjacent to land
  Ocean,   // deep
  Count,
};

// A tile address as a coordinate pair. The engine stores tiles in a flat row-major array
// and passes TileIdx around; Coord exists for map generation, adjacency, and the UI.
struct Coord {
  std::int16_t x{};
  std::int16_t y{};

  friend constexpr bool operator==(Coord, Coord) = default;
};

// Map dimensions travel together because almost every geometric helper needs both, and
// splitting them is how off-by-one errors in row-major indexing get in.
struct MapDims {
  std::int16_t width{};
  std::int16_t height{};

  [[nodiscard]] constexpr int tile_count() const { return width * height; }

  [[nodiscard]] constexpr bool contains(Coord c) const {
    return c.x >= 0 && c.y >= 0 && c.x < width && c.y < height;
  }

  [[nodiscard]] constexpr TileIdx index(Coord c) const {
    return static_cast<TileIdx>(c.y * width + c.x);
  }

  [[nodiscard]] constexpr Coord coord(TileIdx t) const {
    return Coord{static_cast<std::int16_t>(t % width), static_cast<std::int16_t>(t / width)};
  }

  friend constexpr bool operator==(MapDims, MapDims) = default;
};

// THE DEFAULT GAME. One definition, because three structs used to declare this independently
// and they disagreed: GameConfig said 16x16 with four tribes (right), while MatchConfig and
// MapConfig each said 11x11 with two (the competitive duel). The measurement harness therefore
// defaulted to a format nobody plays, and every result had to be read against a setup nobody
// had stated. A default that is only correct in one of the three places it is written is not
// a default, it is a coincidence.
//
// The 11x11 two-player duel is still the COMPETITIVE format and is what the agent is
// ultimately trained for. It is reachable explicitly -- MatchConfig::paper_2020(), or --paper
// on the CLI -- and it should be explicit, because the setup you have to remember to ask for
// is the setup you forget.
inline constexpr MapDims kDefaultMapDims{16, 16};
inline constexpr int kDefaultPlayerCount = 4;

// The 2020 AIIDE paper's experimental setup, kept nameable so a run that intends to reproduce
// it says so rather than inheriting it by accident.
inline constexpr MapDims kCompetitiveMapDims{11, 11};
inline constexpr int kCompetitivePlayerCount = 2;

// Polytopia uses 8-way adjacency: units move diagonally, and the "adjacent" that building
// bonuses and city borders are defined in terms of includes diagonals. Encoding this once
// here rather than per-call-site is deliberate; a 4-way slip would be silent and pervasive.
inline constexpr int kNeighbourCount = 8;
inline constexpr Coord kNeighbourOffsets[kNeighbourCount] = {
    {-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1},
};

}  // namespace poly
