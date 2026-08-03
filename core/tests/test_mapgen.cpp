// Map generation invariants.
//
// Almost every check here is a rule from live-rules.md §6.2 / §10.4 / §10.5 restated as a
// property, because map generation is the one subsystem whose output nobody reads by eye.
// A generator that quietly stops honouring "villages are never adjacent" produces maps that
// look fine, train fine, and are not the game.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "poly/mapgen.hpp"
#include "poly/state.hpp"
#include "testing.hpp"

using namespace poly;

namespace {

std::uint64_t hash_tiles(const GameState& s) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(s.tiles);
  std::uint64_t h = 1469598103934665603ULL;
  for (std::size_t i = 0; i < sizeof(s.tiles); ++i) {
    h ^= bytes[i];
    h *= 1099511628211ULL;
  }
  return h;
}

std::uint64_t hash_state(const GameState& s) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(&s);
  std::uint64_t h = 1469598103934665603ULL;
  for (std::size_t i = 0; i < sizeof(GameState); ++i) {
    h ^= bytes[i];
    h *= 1099511628211ULL;
  }
  return h;
}

int cheb(Coord a, Coord b) {
  const int dx = a.x > b.x ? a.x - b.x : b.x - a.x;
  const int dy = a.y > b.y ? a.y - b.y : b.y - a.y;
  return dx > dy ? dx : dy;
}

bool on_edge(MapDims d, Coord c) {
  return c.x == 0 || c.y == 0 || c.x == d.width - 1 || c.y == d.height - 1;
}

bool is_water(Terrain t) { return t == Terrain::Water || t == Terrain::Ocean; }

MapConfig make_config(int width, int height, int players, std::uint64_t seed) {
  MapConfig cfg;
  cfg.dims = MapDims{static_cast<std::int16_t>(width), static_cast<std::int16_t>(height)};
  cfg.seed = seed;
  cfg.player_count = players;
  for (int p = 0; p < players; ++p) {
    cfg.tribes[p] = static_cast<Tribe>(p % kTribeCount);
  }
  return cfg;
}

// Every structural rule in one pass, so the fuzz test and the targeted tests cannot drift.
void check_all_invariants(const GameState& s, const MapConfig& cfg, const char* label) {
  const MapDims d = s.dims;
  const int n = d.tile_count();
  const auto fail = [&](const std::string& what) {
    ::poly::testing::report_failure(__FILE__, __LINE__, std::string(label) + ": " + what);
  };

  std::vector<Coord> capitals;
  std::vector<Coord> villages;
  std::vector<int> capital_count(static_cast<std::size_t>(kMaxPlayers), 0);
  std::vector<int> unit_count(static_cast<std::size_t>(kMaxPlayers), 0);

  for (int i = 0; i < s.city_count; ++i) {
    const City& c = s.cities[i];
    if (!c.has(CityFlag::Capital)) continue;
    capitals.push_back(d.coord(c.tile));
    capital_count[static_cast<std::size_t>(c.owner)] += 1;
  }
  for (int i = 0; i < s.unit_count; ++i) {
    unit_count[static_cast<std::size_t>(s.units[i].owner)] += 1;
  }

  // Exactly one capital and one starting unit per player, and the unit stands on the capital.
  for (int p = 0; p < cfg.player_count; ++p) {
    if (capital_count[static_cast<std::size_t>(p)] != 1) fail("player lacks exactly one capital");
    if (unit_count[static_cast<std::size_t>(p)] != 1) fail("player lacks exactly one unit");
    if (s.units[p].tile != s.cities[p].tile) fail("starting unit is not on its capital");
    if (s.units[p].hp <= 0) fail("starting unit has no hp");
    // resolutions.md R12.
    const int want_level = cfg.tribes[p] == Tribe::Luxidoor ? 3 : 1;
    if (s.cities[p].level != want_level) fail("wrong capital level");
  }

  // Capitals: never on a map edge, never adjacent to each other.
  for (std::size_t a = 0; a < capitals.size(); ++a) {
    if (on_edge(d, capitals[a])) fail("capital on a map edge");
    for (std::size_t b = a + 1; b < capitals.size(); ++b) {
      if (cheb(capitals[a], capitals[b]) < 2) fail("capitals are adjacent");
    }
  }

  for (int i = 0; i < n; ++i) {
    const Tile& t = s.tiles[i];
    const Coord c = d.coord(static_cast<TileIdx>(i));

    // Deep water shares no edge with land; shallow water shares at least one.
    const bool touches_land = mapgen::shares_edge_with_land(s, static_cast<TileIdx>(i));
    if (t.terrain == Terrain::Ocean && touches_land) fail("Ocean tile shares an edge with land");
    if (t.terrain == Terrain::Water && !touches_land) fail("Water tile shares no edge with land");

    // Territory: an owned tile always names the city that owns it, and that city agrees.
    if (t.owner != kNoPlayer) {
      if (t.city == kNoCity) fail("owned tile has no owning city");
      else if (s.cities[t.city].owner != t.owner) fail("tile owner disagrees with its city");
    } else if (t.city != kNoCity) {
      fail("unowned tile names a city");
    }

    if (t.resource == Resource::Village) {
      if (on_edge(d, c)) fail("village on a map edge");
      villages.push_back(c);
    }

    // §6.2: all resources spawn only within two tiles of a city or village. Lighthouses are
    // the documented exception -- they are pinned to the corners.
    if (t.resource != Resource::None && t.resource != Resource::Lighthouse &&
        t.resource != Resource::Village) {
      int best = 1 << 20;
      for (const Coord cap : capitals) best = std::min(best, cheb(c, cap));
      for (int j = 0; j < n; ++j) {
        if (s.tiles[j].resource == Resource::Village) {
          best = std::min(best, cheb(c, d.coord(static_cast<TileIdx>(j))));
        }
      }
      if (best > 2) fail("resource more than two tiles from a city or village");
    }

    if (t.resource == Resource::Ruin) {
      if (t.terrain == Terrain::Water) fail("ruin on shallow water");
      for (const Coord off : kNeighbourOffsets) {
        const Coord nb{static_cast<std::int16_t>(c.x + off.x),
                       static_cast<std::int16_t>(c.y + off.y)};
        if (!d.contains(nb)) continue;
        if (s.tiles[d.index(nb)].resource == Resource::Ruin) fail("ruin adjacent to a ruin");
        for (const Coord cap : capitals) {
          if (cap == nb) fail("ruin adjacent to a capital");
        }
      }
      for (const Coord cap : capitals) {
        if (cap == c) fail("ruin on a capital");
      }
    }

    if (t.resource == Resource::Starfish) {
      if (!is_water(t.terrain)) fail("starfish on land");
      for (const Coord off : kNeighbourOffsets) {
        const Coord nb{static_cast<std::int16_t>(c.x + off.x),
                       static_cast<std::int16_t>(c.y + off.y)};
        if (!d.contains(nb)) continue;
        const Resource r = s.tiles[d.index(nb)].resource;
        if (r == Resource::Starfish) fail("starfish adjacent to a starfish");
        if (r == Resource::Lighthouse) fail("starfish adjacent to a lighthouse");
        if (r == Resource::Village) fail("starfish adjacent to a village");
        for (const Coord cap : capitals) {
          if (cap == nb) fail("starfish adjacent to a city");
        }
      }
    }

    // Terrain/resource agreement.
    switch (t.resource) {
      case Resource::Fruit:
      case Resource::Crop:
        if (t.terrain != Terrain::Field) fail("field resource off a field");
        break;
      case Resource::Game:
        if (t.terrain != Terrain::Forest) fail("game off a forest");
        break;
      case Resource::Metal:
        if (t.terrain != Terrain::Mountain) fail("metal off a mountain");
        break;
      case Resource::Fish:
        if (!is_water(t.terrain)) fail("fish on land");
        break;
      case Resource::Village:
        if (is_water(t.terrain)) fail("village on water");
        break;
      default:
        break;
    }
  }

  // No more than one village per 3x3 area, and villages keep clear of capitals.
  for (std::size_t a = 0; a < villages.size(); ++a) {
    for (std::size_t b = a + 1; b < villages.size(); ++b) {
      if (cheb(villages[a], villages[b]) < 2) fail("two villages within a 3x3");
    }
    for (const Coord cap : capitals) {
      if (cheb(villages[a], cap) < 2) fail("village adjacent to a capital");
    }
  }

  // Fog: exactly the 3x3 around each capital is revealed (resolutions.md R18).
  for (int p = 0; p < cfg.player_count; ++p) {
    const Coord cap = d.coord(s.cities[p].tile);
    for (int i = 0; i < n; ++i) {
      const Coord c = d.coord(static_cast<TileIdx>(i));
      const bool want = cheb(c, cap) <= kInitialVisionRadius;
      if (s.explored[p].test(static_cast<TileIdx>(i)) != want) fail("initial fog is wrong");
    }
  }
}

}  // namespace

TEST("mapgen: the same config produces a bit-identical state") {
  // The property the whole replay and golden-trace story rests on. Hashing the entire
  // GameState, not just the tiles, catches an rng that advanced a different number of times.
  const MapConfig cfg = make_config(11, 11, 2, 12345);
  GameState a{};
  GameState b{};
  std::string error;
  CHECK(generate_map(cfg, a, error));
  CHECK(generate_map(cfg, b, error));
  CHECK_EQ(hash_tiles(a), hash_tiles(b));
  CHECK_EQ(hash_state(a), hash_state(b));
  CHECK(std::memcmp(&a, &b, sizeof(GameState)) == 0);
}

TEST("mapgen: a different seed produces a different map") {
  std::string error;
  GameState a{};
  GameState b{};
  CHECK(generate_map(make_config(11, 11, 2, 1), a, error));
  CHECK(generate_map(make_config(11, 11, 2, 2), b, error));
  CHECK(hash_tiles(a) != hash_tiles(b));
}

TEST("mapgen: generating into a dirty state still yields the canonical result") {
  // Stale bytes in the unused capacity beyond the live map would break bit-identity for any
  // caller that hashes the struct, so generate_map must reset all of it.
  const MapConfig cfg = make_config(11, 11, 2, 99);
  GameState clean{};
  GameState dirty{};
  std::string error;
  CHECK(generate_map(make_config(32, 32, 8, 7), dirty, error));
  CHECK(generate_map(cfg, clean, error));
  CHECK(generate_map(cfg, dirty, error));
  CHECK(std::memcmp(&clean, &dirty, sizeof(GameState)) == 0);
}

TEST("mapgen: land ratio is honoured across seeds and targets") {
  std::string error;
  const float targets[] = {0.3f, 0.5f, 0.7f};
  for (const float target : targets) {
    for (std::uint64_t seed = 0; seed < 40; ++seed) {
      MapConfig cfg = make_config(16, 16, 3, seed);
      cfg.land_ratio = target;
      GameState s{};
      CHECK(generate_map(cfg, s, error));
      int land = 0;
      for (int i = 0; i < s.dims.tile_count(); ++i) {
        if (!is_water(s.tiles[i].terrain)) ++land;
      }
      const float ratio = static_cast<float>(land) / static_cast<float>(s.dims.tile_count());
      if (ratio < target - 0.02f || ratio > target + 0.02f) {
        ::poly::testing::report_failure(__FILE__, __LINE__,
                                        "land ratio " + std::to_string(ratio) + " != " +
                                            std::to_string(target));
      }
    }
  }
}

TEST("mapgen: deep water is exactly the water that touches no land") {
  std::string error;
  for (std::uint64_t seed = 0; seed < 25; ++seed) {
    MapConfig cfg = make_config(18, 18, 4, seed);
    cfg.land_ratio = 0.45f;
    GameState s{};
    CHECK(generate_map(cfg, s, error));
    for (int i = 0; i < s.dims.tile_count(); ++i) {
      const bool touches = mapgen::shares_edge_with_land(s, static_cast<TileIdx>(i));
      if (s.tiles[i].terrain == Terrain::Ocean) CHECK(!touches);
      if (s.tiles[i].terrain == Terrain::Water) CHECK(touches);
    }
  }
}

TEST("mapgen: lighthouses sit on the four map corners") {
  std::string error;
  GameState s{};
  CHECK(generate_map(make_config(14, 14, 2, 5), s, error));
  const MapDims d = s.dims;
  const Coord corners[4] = {{0, 0},
                            {static_cast<std::int16_t>(d.width - 1), 0},
                            {0, static_cast<std::int16_t>(d.height - 1)},
                            {static_cast<std::int16_t>(d.width - 1),
                             static_cast<std::int16_t>(d.height - 1)}};
  for (const Coord c : corners) {
    CHECK(s.tiles[d.index(c)].resource == Resource::Lighthouse);
  }
  int lighthouses = 0;
  for (int i = 0; i < d.tile_count(); ++i) {
    if (s.tiles[i].resource == Resource::Lighthouse) ++lighthouses;
  }
  CHECK_EQ(lighthouses, 4);
}

TEST("mapgen: territory claims never overlap and are at most 3x3") {
  // resolutions.md R3: first come, first served. Two capitals five tiles apart both get nine
  // tiles; two capitals two apart do not, and the second one is the loser.
  std::string error;
  for (std::uint64_t seed = 0; seed < 30; ++seed) {
    GameState s{};
    const MapConfig cfg = make_config(12, 12, 4, seed);
    CHECK(generate_map(cfg, s, error));

    std::vector<int> claimed(static_cast<std::size_t>(s.city_count), 0);
    for (int i = 0; i < s.dims.tile_count(); ++i) {
      const Tile& t = s.tiles[i];
      if (t.city == kNoCity) continue;
      claimed[static_cast<std::size_t>(t.city)] += 1;
      const Coord a = s.dims.coord(static_cast<TileIdx>(i));
      const Coord b = s.dims.coord(s.cities[t.city].tile);
      CHECK(cheb(a, b) <= 1);  // claims are 3x3, never wider
    }
    for (int c = 0; c < s.city_count; ++c) {
      CHECK(claimed[static_cast<std::size_t>(c)] >= 1);
      CHECK(claimed[static_cast<std::size_t>(c)] <= 9);
      // A city always owns the tile it stands on: it claims first among its own ring.
      CHECK_EQ(s.tiles[s.cities[c].tile].city, static_cast<CityId>(c));
    }
  }
}

TEST("mapgen: every documented map size and player count generates") {
  std::string error;
  for (int size = 9; size <= 32; ++size) {
    for (int players = 2; players <= 8; ++players) {
      MapConfig cfg = make_config(size, size, players, static_cast<std::uint64_t>(size * 100 + players));
      GameState s{};
      if (!generate_map(cfg, s, error)) {
        ::poly::testing::report_failure(__FILE__, __LINE__,
                                        std::to_string(size) + "x" + std::to_string(size) + " / " +
                                            std::to_string(players) + "p failed: " + error);
        continue;
      }
      CHECK_EQ(static_cast<int>(s.city_count), players);
      CHECK_EQ(static_cast<int>(s.unit_count), players);
      check_all_invariants(s, cfg, "sizes");
    }
  }
}

TEST("mapgen: non-square maps generate") {
  std::string error;
  GameState s{};
  const MapConfig cfg = make_config(9, 20, 3, 3);
  CHECK(generate_map(cfg, s, error));
  check_all_invariants(s, cfg, "non-square");
}

TEST("mapgen: impossible configurations are rejected with a reason") {
  std::string error;
  GameState s{};
  CHECK(!generate_map(make_config(2, 2, 1, 0), s, error));
  CHECK(!error.empty());
  CHECK(!generate_map(make_config(33, 11, 2, 0), s, error));
  CHECK(!error.empty());
  CHECK(!generate_map(make_config(11, 11, 0, 0), s, error));
  CHECK(!error.empty());
  // Four interior tiles cannot hold five non-adjacent capitals.
  CHECK(!generate_map(make_config(4, 4, 5, 0), s, error));
  CHECK(!error.empty());

  MapConfig bad_ratio = make_config(11, 11, 2, 0);
  bad_ratio.land_ratio = 1.5f;
  CHECK(!generate_map(bad_ratio, s, error));
}

TEST("mapgen: fixed starts are honoured and validated") {
  std::string error;
  MapConfig cfg = make_config(11, 11, 2, 4);
  cfg.fixed_starts = true;
  cfg.start_tiles[0] = cfg.dims.index({2, 2});
  cfg.start_tiles[1] = cfg.dims.index({8, 8});
  GameState s{};
  CHECK(generate_map(cfg, s, error));
  CHECK_EQ(s.cities[0].tile, cfg.dims.index({2, 2}));
  CHECK_EQ(s.cities[1].tile, cfg.dims.index({8, 8}));
  check_all_invariants(s, cfg, "fixed starts");

  cfg.start_tiles[1] = cfg.dims.index({3, 2});  // adjacent
  CHECK(!generate_map(cfg, s, error));
  cfg.start_tiles[1] = cfg.dims.index({0, 5});  // on an edge
  CHECK(!generate_map(cfg, s, error));
}

TEST("mapgen: tribe bias moves terrain in the documented direction") {
  // §10.4. Not an exact-rate assertion -- the quota is per-region and regions are ragged --
  // but Bardur/Hoodrick forest against Oumaji forest is a 7x gap and must survive averaging.
  std::string error;
  int oumaji_forest = 0;
  int hoodrick_forest = 0;
  int vengir_metal = 0;
  int imperius_metal = 0;
  for (std::uint64_t seed = 0; seed < 30; ++seed) {
    MapConfig cfg = make_config(16, 16, 2, seed);
    cfg.land_ratio = 0.8f;
    cfg.tribes[0] = Tribe::Oumaji;
    cfg.tribes[1] = Tribe::Hoodrick;
    GameState s{};
    CHECK(generate_map(cfg, s, error));
    const Coord a = s.dims.coord(s.cities[0].tile);
    const Coord b = s.dims.coord(s.cities[1].tile);
    for (int i = 0; i < s.dims.tile_count(); ++i) {
      if (s.tiles[i].terrain != Terrain::Forest) continue;
      const Coord c = s.dims.coord(static_cast<TileIdx>(i));
      if (cheb(c, a) < cheb(c, b)) ++oumaji_forest;
      else if (cheb(c, b) < cheb(c, a)) ++hoodrick_forest;
    }

    MapConfig m = make_config(16, 16, 2, seed);
    m.land_ratio = 0.8f;
    m.tribes[0] = Tribe::Vengir;
    m.tribes[1] = Tribe::Imperius;
    GameState t{};
    CHECK(generate_map(m, t, error));
    const Coord va = t.dims.coord(t.cities[0].tile);
    const Coord vb = t.dims.coord(t.cities[1].tile);
    for (int i = 0; i < t.dims.tile_count(); ++i) {
      if (t.tiles[i].resource != Resource::Metal) continue;
      const Coord c = t.dims.coord(static_cast<TileIdx>(i));
      if (cheb(c, va) < cheb(c, vb)) ++vengir_metal;
      else if (cheb(c, vb) < cheb(c, va)) ++imperius_metal;
    }
  }
  CHECK(hoodrick_forest > oumaji_forest * 3);
  CHECK(vengir_metal >= imperius_metal);
}

TEST("mapgen: fuzz 200 seeds without a crash or a broken invariant") {
  std::string error;
  for (std::uint64_t seed = 0; seed < 200; ++seed) {
    const int size = 9 + static_cast<int>(seed % 20);
    const int players = 2 + static_cast<int>(seed % 7);
    MapConfig cfg = make_config(size, size, players, seed);
    cfg.land_ratio = 0.25f + static_cast<float>(seed % 11) * 0.05f;
    for (int p = 0; p < players; ++p) {
      cfg.tribes[p] = static_cast<Tribe>((static_cast<int>(seed) + p) % kTribeCount);
    }
    GameState s{};
    if (!generate_map(cfg, s, error)) {
      ::poly::testing::report_failure(__FILE__, __LINE__, "fuzz seed failed: " + error);
      continue;
    }
    check_all_invariants(s, cfg, "fuzz");
  }
}

TEST("mapgen: a fixed JSON map loads exactly as written") {
  const char* path = "poly_test_fixed_map.json";
  {
    std::ofstream out(path);
    out << R"({
      "width": 7, "height": 7, "seed": 11,
      "terrain": [
        "ooooooo",
        "ow...wo",
        "o..f..o",
        "o.m.m.o",
        "o..f..o",
        "ow...wo",
        "ooooooo"
      ],
      "resources": [
        "L.....L",
        ".......",
        "..G....",
        "..M.M..",
        "....V..",
        ".......",
        "L.....L"
      ],
      "players": [
        {"tribe": "bardur", "capital": [2, 2]},
        {"tribe": "luxidoor", "capital": 30}
      ]
    })";
  }

  GameState s{};
  std::string error;
  const bool ok = load_fixed_map(path, s, error);
  if (!ok) ::poly::testing::report_failure(__FILE__, __LINE__, "load failed: " + error);
  std::remove(path);
  if (!ok) return;

  CHECK_EQ(static_cast<int>(s.dims.width), 7);
  CHECK_EQ(static_cast<int>(s.dims.height), 7);
  CHECK_EQ(static_cast<int>(s.player_count), 2);
  CHECK_EQ(s.cities[0].tile, s.dims.index({2, 2}));
  CHECK_EQ(s.cities[1].tile, static_cast<TileIdx>(30));  // 7*4 + 2 = (2, 4)
  CHECK_EQ(static_cast<int>(s.cities[0].level), 1);
  CHECK_EQ(static_cast<int>(s.cities[1].level), 3);  // Luxidoor, R12
  CHECK(s.units[0].type == UnitType::Warrior);
  CHECK(s.tiles[s.dims.index({3, 2})].terrain == Terrain::Forest);
  CHECK(s.tiles[s.dims.index({2, 3})].resource == Resource::Metal);
  CHECK(s.tiles[s.dims.index({4, 4})].resource == Resource::Village);
  CHECK(s.tiles[s.dims.index({0, 0})].resource == Resource::Lighthouse);

  // Water is always reclassified, so the fixture cannot encode an inconsistent sea.
  CHECK(s.tiles[s.dims.index({0, 0})].terrain == Terrain::Ocean);
  CHECK(s.tiles[s.dims.index({1, 1})].terrain == Terrain::Water);

  MapConfig cfg = make_config(7, 7, 2, 11);
  cfg.tribes[0] = Tribe::Bardur;
  cfg.tribes[1] = Tribe::Luxidoor;
  check_all_invariants(s, cfg, "fixed map");

  // Loading twice gives the same bytes.
  GameState again{};
  {
    std::ofstream out(path);
    out << R"({"width": 3, "height": 3, "seed": 1,
                "terrain": ["...", "...", "..."],
                "players": [{"tribe": "kickoo", "capital": [1, 1]}]})";
  }
  CHECK(load_fixed_map(path, again, error));
  GameState again2{};
  CHECK(load_fixed_map(path, again2, error));
  std::remove(path);
  CHECK(std::memcmp(&again, &again2, sizeof(GameState)) == 0);
}

TEST("mapgen: a malformed fixed map is rejected with a reason") {
  const char* path = "poly_test_bad_map.json";
  {
    std::ofstream out(path);
    out << R"({"width": 4, "height": 2, "terrain": ["....."],
                "players": [{"tribe": "bardur", "capital": [1, 1]}]})";
  }
  GameState s{};
  std::string error;
  CHECK(!load_fixed_map(path, s, error));
  CHECK(!error.empty());
  std::remove(path);

  CHECK(!load_fixed_map("no_such_file_anywhere.json", s, error));
  CHECK(!error.empty());
}
