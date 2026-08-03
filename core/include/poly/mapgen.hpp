// Procedural map generation.
//
// The algorithm follows the QuasiStellar Polytopia-Map-Generator pipeline that both the Java
// Tribes framework and PolyEnv derive from, because it is the community standard and its
// output distributions match the real game closely enough to train against:
//
//   capitals -> villages -> land/water split -> deep/shallow -> forest/mountain quotas
//   -> territory -> lighthouses -> resources -> starfish -> ruins -> initial fog
//
// (Generation order is live-rules.md §10.5; the terrain and resource rates are §10.4, the
// spawn constraints §6.2.)
//
// DETERMINISM IS ABSOLUTE. Everything random here is drawn from `state.rng`, a Pcg32 seeded
// from MapConfig::seed. Nothing in this translation unit may touch rand(), std::random_device,
// the clock, a hash of a pointer, or any global generator: identical MapConfig must produce a
// bit-identical GameState on every platform, which is what makes replays and the golden-trace
// harness possible (docs/phase0-plan.md §1.4).

#pragma once

#include <cstdint>
#include <string>

#include "poly/content.hpp"
#include "poly/state.hpp"
#include "poly/types.hpp"

namespace poly {

struct MapConfig {
  MapDims dims{11, 11};
  std::uint64_t seed{0};
  float land_ratio{0.5f};  // fraction of tiles that end up land; honoured as an exact quota
  int player_count{2};
  Tribe tribes[kMaxPlayers]{};

  // Regression fixtures pin the capitals instead of letting the spread heuristic choose.
  bool fixed_starts{false};
  TileIdx start_tiles[kMaxPlayers]{};
};

// resolutions.md R18: at game start a player sees a 1-tile radius around their capital, and
// that initial reveal is the ONLY city-based vision they will ever get. Named because
// live-rules.md §10.6 claims 25 starting tiles (a 5x5) and the two sources disagree; R18 is
// the owner ruling and therefore wins.
inline constexpr int kInitialVisionRadius = 1;

// Fills terrain, resources, villages, capitals, starting units, starting territory and each
// player's initial explored set. Returns false and fills `error` on an impossible config
// (map too small for the requested players, bad dimensions, unusable land ratio).
//
// On success `state` is fully overwritten -- including the unused capacity beyond the live
// map -- so that two states generated from equal configs are byte-identical.
bool generate_map(const MapConfig& cfg, GameState& state, std::string& error);

// Reads a hand-written JSON map so specific positions become permanent regression fixtures.
// Format (see data/maps):
//
//   {
//     "width": 5, "height": 5, "seed": 1,
//     "terrain":   [".....", ".ff..", ".m...", "wwww.", "oooow"],
//     "resources": [".....", "..G..", "...V.", ".i...", "....."],
//     "players":   [{"tribe": "bardur", "capital": [1, 1]}]
//   }
//
// terrain chars:   '.' field  'f' forest  'm' mountain  'w'/'o'/'~' water
// resource chars:  '.' none  'F' fruit  'C' crop  'G' game  'i' fish  'S' starfish
//                  'M' metal  'R' ruin  'V' village  'L' lighthouse
//
// Water is ALWAYS reclassified into shallow/deep by the engine rule (deep water shares no
// edge with any land tile), so a fixture cannot encode an internally inconsistent map.
// "capital" accepts either [x, y] or a flat tile index.
bool load_fixed_map(const std::string& path, GameState& state, std::string& error);

namespace mapgen {

// The shallow/deep predicate, exported so tests and rules code cannot drift from generation.
// "Edge" is literal: the four orthogonal neighbours. Diagonals share a corner, not an edge
// (live-rules.md §6.1, "shares >=1 edge with land"), even though movement adjacency is 8-way.
[[nodiscard]] bool shares_edge_with_land(const GameState& state, TileIdx t);

[[nodiscard]] bool is_water(Terrain t);

}  // namespace mapgen
}  // namespace poly
