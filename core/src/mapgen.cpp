#include "poly/mapgen.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

#include "poly/json.hpp"

namespace poly {
namespace {

// ---------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------

inline std::size_t sz(int i) { return static_cast<std::size_t>(i); }

inline int cheb(Coord a, Coord b) {
  const int dx = std::abs(static_cast<int>(a.x) - static_cast<int>(b.x));
  const int dy = std::abs(static_cast<int>(a.y) - static_cast<int>(b.y));
  return dx > dy ? dx : dy;
}

// The four orthogonal offsets. kNeighbourOffsets is 8-way because movement is; edge-sharing
// is a strictly different relation and conflating them is how "every Water tile touches land"
// quietly becomes false.
constexpr Coord kEdgeOffsets[4] = {{0, -1}, {-1, 0}, {1, 0}, {0, 1}};

// Fisher-Yates through the state rng. Used wherever a rule is "place N of these somewhere
// legal": shuffling once and walking the list keeps placement unbiased without a rejection
// loop that could spin on a nearly-full map.
template <typename T>
void shuffle(std::vector<T>& v, Pcg32& rng) {
  for (std::size_t i = v.size(); i > 1; --i) {
    const std::uint32_t j = rng.bounded(static_cast<std::uint32_t>(i));
    std::swap(v[i - 1], v[sz(static_cast<int>(j))]);
  }
}

// ---------------------------------------------------------------------------------------
// Tribe data (live-rules.md §10.4, §10.1, §2.1)
// ---------------------------------------------------------------------------------------
// These duplicate numbers that will eventually live in data/rules/live.json. They are here
// because map generation runs before Rules is loaded and needs only seven scalars per tribe;
// when the rules loader lands, this table is the one to delete.

struct TribeBias {
  float mountain;
  float forest;
  float fruit;
  float crop;
  float animal;
  float metal;
  float fish;
};

// Water multipliers (Oumaji 0.5, Kickoo 2.0) are deliberately absent: §10.4 footnotes both as
// dead values that the shipping game never applies. Implementing them would make our maps
// diverge from the real ones in the direction of a bug we know about.
constexpr TribeBias kTribeBias[kTribeCount] = {
    /* XinXi    */ {1.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.5f, 1.0f},
    /* Imperius */ {1.0f, 1.0f, 2.0f, 1.0f, 0.5f, 1.0f, 1.0f},
    /* Bardur   */ {1.0f, 0.8f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f},
    /* Oumaji   */ {0.5f, 0.2f, 1.0f, 1.0f, 0.2f, 1.0f, 1.0f},
    /* Kickoo   */ {0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.5f},
    /* Hoodrick */ {0.5f, 1.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    /* Luxidoor */ {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    /* Vengir   */ {1.0f, 1.0f, 0.1f, 1.0f, 0.1f, 2.0f, 0.1f},
    /* Zebasi   */ {0.5f, 0.5f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f},
    /* AiMo     */ {1.5f, 1.0f, 1.0f, 0.1f, 1.0f, 1.0f, 1.0f},
    /* Quetzali */ {1.0f, 1.0f, 2.0f, 0.1f, 1.0f, 1.0f, 1.0f},
    /* Yadakk   */ {0.5f, 0.5f, 1.5f, 1.0f, 1.0f, 1.0f, 1.0f},
};

// §10.4: the tribe that always spawns with this resource in its capital ring, force-spawned
// if world generation did not provide it.
constexpr Resource kGuaranteedResource[kTribeCount] = {
    Resource::Metal, Resource::Fruit, Resource::Game,  Resource::Fruit,
    Resource::Fish,  Resource::Game,  Resource::Game,  Resource::Game,
    Resource::Crop,  Resource::Game,  Resource::Fruit, Resource::Fruit,
};

// §2.1: "Oumaji -> Rider, Hoodrick -> Archer, Vengir -> Swordsman, Ai-Mo -> Mind Bender,
// Quetzali -> Defender; every other regular tribe -> Warrior."
constexpr UnitType kStartingUnit[kTribeCount] = {
    UnitType::Warrior, UnitType::Warrior,  UnitType::Warrior, UnitType::Rider,
    UnitType::Warrior, UnitType::Archer,   UnitType::Warrior, UnitType::Swordsman,
    UnitType::Warrior, UnitType::MindBender, UnitType::Defender, UnitType::Warrior,
};

int starting_unit_hp(UnitType t) {
  switch (t) {
    case UnitType::Defender:
    case UnitType::Swordsman:
      return 15;
    default:
      return 10;
  }
}

// resolutions.md R12: Luxidoor is the only regular tribe whose capital does not start at 1.
int capital_level(Tribe t) { return t == Tribe::Luxidoor ? 3 : 1; }

const TribeBias& bias_for(Tribe t) {
  const int i = static_cast<int>(t);
  return kTribeBias[(i >= 0 && i < kTribeCount) ? i : static_cast<int>(Tribe::Luxidoor)];
}

// ---------------------------------------------------------------------------------------
// Base terrain and resource rates (§10.4). These are Luxidoor's -- Luxidoor is the baseline.
// ---------------------------------------------------------------------------------------

constexpr float kBaseMountain = 0.14f;
constexpr float kBaseForest = 0.38f;
constexpr float kBaseField = 0.48f;

// Conditional on terrain type, split out of the §10.4 table: 18/48 fruit inside a city ring,
// 6/48 outside, and so on. Keeping them conditional is what lets the terrain quota and the
// resource roll stay independent.
constexpr float kInnerFruit = 18.0f / 48.0f;
constexpr float kInnerCrop = 18.0f / 48.0f;
constexpr float kOuterFruit = 6.0f / 48.0f;
constexpr float kOuterCrop = 6.0f / 48.0f;
constexpr float kInnerAnimal = 19.0f / 38.0f;
constexpr float kOuterAnimal = 6.0f / 38.0f;
constexpr float kInnerMetal = 11.0f / 14.0f;
constexpr float kOuterMetal = 3.0f / 14.0f;
constexpr float kFishRate = 0.5f;          // §10.4: 50% of shallow water
constexpr float kStarfishPerWater = 1.0f / 25.0f;
constexpr float kVillageDensity = 0.3f;    // §10.5 density coefficient (Lakes / Archipelago)
constexpr int kResourceRange = 2;          // §6.2: resources only within two tiles of a city

// §10.5 ruin counts by map size. Interpolated between the documented sizes so that any
// dimension in [3, 32] gets a sensible count rather than a magic constant.
constexpr int kRuinSizes[6] = {121, 196, 256, 324, 400, 900};
constexpr int kRuinCounts[6] = {4, 5, 7, 9, 11, 23};

int ruin_count_for(int tiles) {
  if (tiles <= kRuinSizes[0]) {
    const int scaled = (tiles * kRuinCounts[0] + kRuinSizes[0] / 2) / kRuinSizes[0];
    return scaled < 1 ? 1 : scaled;
  }
  for (int i = 1; i < 6; ++i) {
    if (tiles <= kRuinSizes[i]) {
      const int span = kRuinSizes[i] - kRuinSizes[i - 1];
      const int rise = kRuinCounts[i] - kRuinCounts[i - 1];
      return kRuinCounts[i - 1] + ((tiles - kRuinSizes[i - 1]) * rise + span / 2) / span;
    }
  }
  return kRuinCounts[5];
}

// ---------------------------------------------------------------------------------------
// Noise
// ---------------------------------------------------------------------------------------
// Value noise plus box smoothing, exactly as the QuasiStellar generator does it. Smoothing is
// the entire reason coastlines and forest belts look like terrain instead of static: an
// unsmoothed threshold on white noise gives salt-and-pepper water that no real map has.

std::vector<float> smoothed_noise(Pcg32& rng, MapDims dims, int passes) {
  const int n = dims.tile_count();
  std::vector<float> a(sz(n));
  for (int i = 0; i < n; ++i) a[sz(i)] = rng.unit_float();

  std::vector<float> b(sz(n));
  for (int p = 0; p < passes; ++p) {
    for (int y = 0; y < dims.height; ++y) {
      for (int x = 0; x < dims.width; ++x) {
        const Coord c{static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)};
        float sum = a[sz(dims.index(c))] * 2.0f;
        float weight = 2.0f;
        for (const Coord off : kNeighbourOffsets) {
          const Coord nb{static_cast<std::int16_t>(x + off.x), static_cast<std::int16_t>(y + off.y)};
          if (!dims.contains(nb)) continue;
          sum += a[sz(dims.index(nb))];
          weight += 1.0f;
        }
        b[sz(dims.index(c))] = sum / weight;
      }
    }
    a.swap(b);
  }
  return a;
}

// ---------------------------------------------------------------------------------------
// Working set
// ---------------------------------------------------------------------------------------

struct Gen {
  const MapConfig* cfg{};
  GameState* st{};
  MapDims dims{};
  int n{0};
  std::vector<TileIdx> capitals;  // one per player, in player order
  std::vector<TileIdx> villages;
  std::vector<int> city_dist;     // Chebyshev distance to the nearest city or village
  std::vector<int> region;        // index of the nearest capital, for tribe biasing

  [[nodiscard]] Coord at(TileIdx t) const { return dims.coord(t); }
  [[nodiscard]] Tile& tile(TileIdx t) const { return st->tiles[t]; }
  [[nodiscard]] bool on_edge(Coord c) const {
    return c.x == 0 || c.y == 0 || c.x == dims.width - 1 || c.y == dims.height - 1;
  }
};

bool is_water_terrain(Terrain t) { return t == Terrain::Water || t == Terrain::Ocean; }

// ---------------------------------------------------------------------------------------
// Step 1: capitals (§10.5 -- capitals are placed before anything else)
// ---------------------------------------------------------------------------------------
// Greedy farthest-point sampling over the interior. Each new capital goes wherever the
// minimum distance to every already-placed capital is largest, which spreads N points across
// an arbitrary rectangle without needing the quadrant scheme's per-player-count special cases.
// Ties are broken by an rng draw, so two players never get a mirror-image start on the same
// seed just because the scan order favoured low indices.

bool place_capitals(Gen& g, std::string& error) {
  const MapConfig& cfg = *g.cfg;
  const int players = cfg.player_count;
  g.capitals.assign(sz(players), kNoTile);

  // Capitals are never on a map edge: an edge capital's 3x3 territory would be clipped, and
  // the game does not produce them.
  std::vector<TileIdx> interior;
  for (int y = 1; y < g.dims.height - 1; ++y) {
    for (int x = 1; x < g.dims.width - 1; ++x) {
      interior.push_back(g.dims.index({static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)}));
    }
  }
  if (static_cast<int>(interior.size()) < players) {
    error = "map is too small for " + std::to_string(players) + " capitals";
    return false;
  }

  if (cfg.fixed_starts) {
    for (int p = 0; p < players; ++p) {
      const TileIdx t = cfg.start_tiles[p];
      if (t < 0 || t >= static_cast<TileIdx>(g.n)) {
        error = "fixed start tile out of range for player " + std::to_string(p);
        return false;
      }
      if (g.on_edge(g.at(t))) {
        error = "fixed start tile on a map edge for player " + std::to_string(p);
        return false;
      }
      for (int q = 0; q < p; ++q) {
        if (cheb(g.at(t), g.at(g.capitals[sz(q)])) < 2) {
          error = "fixed start tiles are adjacent (players " + std::to_string(q) + " and " +
                  std::to_string(p) + ")";
          return false;
        }
      }
      g.capitals[sz(p)] = t;
    }
    return true;
  }

  g.capitals[0] = interior[sz(static_cast<int>(g.st->rng.bounded(
      static_cast<std::uint32_t>(interior.size()))))];

  std::vector<TileIdx> best;
  for (int p = 1; p < players; ++p) {
    int best_score = -1;
    best.clear();
    for (const TileIdx cand : interior) {
      int nearest = 1 << 30;
      for (int q = 0; q < p; ++q) {
        nearest = std::min(nearest, cheb(g.at(cand), g.at(g.capitals[sz(q)])));
      }
      if (nearest > best_score) {
        best_score = nearest;
        best.clear();
        best.push_back(cand);
      } else if (nearest == best_score) {
        best.push_back(cand);
      }
    }
    if (best_score < 2) {
      error = "map is too small to keep " + std::to_string(players) +
              " capitals non-adjacent";
      return false;
    }
    g.capitals[sz(p)] = best[sz(static_cast<int>(
        g.st->rng.bounded(static_cast<std::uint32_t>(best.size()))))];
  }
  return true;
}

// ---------------------------------------------------------------------------------------
// Step 2: villages (§10.5 -- placed before terrain)
// ---------------------------------------------------------------------------------------
// Count from the documented pre-terrain formula:
//   ((floor(width / 3))^2 - capitals) * densityCoefficient
// Constraints from §6.2: never on a map edge, and no more than one per 3x3 area (i.e. any two
// villages are at least 2 apart). Capitals count as occupied for the same reason.

void place_villages(Gen& g) {
  const int grid = g.dims.width / 3;
  const int budget = grid * grid - static_cast<int>(g.capitals.size());
  int want = 0;
  if (budget > 0) {
    want = static_cast<int>(static_cast<float>(budget) * kVillageDensity + 0.5f);
  }
  if (want <= 0) return;

  std::vector<TileIdx> candidates;
  for (int y = 1; y < g.dims.height - 1; ++y) {
    for (int x = 1; x < g.dims.width - 1; ++x) {
      candidates.push_back(g.dims.index({static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)}));
    }
  }
  shuffle(candidates, g.st->rng);

  for (const TileIdx t : candidates) {
    if (static_cast<int>(g.villages.size()) >= want) break;
    bool ok = true;
    for (const TileIdx c : g.capitals) {
      if (cheb(g.at(t), g.at(c)) < 2) { ok = false; break; }
    }
    if (ok) {
      for (const TileIdx v : g.villages) {
        if (cheb(g.at(t), g.at(v)) < 2) { ok = false; break; }
      }
    }
    if (ok) g.villages.push_back(t);
  }
  // Deterministic regardless of the shuffle's output order, so that a fixture diff of the
  // village list reads in map order.
  std::sort(g.villages.begin(), g.villages.end());
}

// ---------------------------------------------------------------------------------------
// Step 3: land / water split, then deep / shallow
// ---------------------------------------------------------------------------------------
// land_ratio is honoured as an exact QUOTA, not a per-tile probability: the tiles with the
// lowest smoothed noise become water until the quota is met. A per-tile roll would make the
// realised ratio a binomial around the target, which is fine for a game and useless for a
// test that has to distinguish "the ratio is respected" from "the ratio is ignored".

void split_land_and_water(Gen& g) {
  const MapConfig& cfg = *g.cfg;
  std::vector<float> height = smoothed_noise(g.st->rng, g.dims, 3);

  std::vector<bool> forced_land(sz(g.n), false);
  for (const TileIdx t : g.capitals) forced_land[sz(t)] = true;
  for (const TileIdx t : g.villages) forced_land[sz(t)] = true;

  // A capital whose whole ring is water is unplayable, so the eight tiles around one get a
  // thumb on the scale -- not a guarantee, because Kickoo starts are genuinely coastal.
  for (const TileIdx t : g.capitals) {
    const Coord c = g.at(t);
    for (const Coord off : kNeighbourOffsets) {
      const Coord nb{static_cast<std::int16_t>(c.x + off.x), static_cast<std::int16_t>(c.y + off.y)};
      if (g.dims.contains(nb)) height[sz(g.dims.index(nb))] += 0.30f;
    }
  }
  // Mild coastal bias: real maps are ringed by sea, and it puts the lighthouses somewhere
  // plausible.
  for (int y = 0; y < g.dims.height; ++y) {
    for (int x = 0; x < g.dims.width; ++x) {
      const int edge = std::min(std::min(x, y), std::min(g.dims.width - 1 - x, g.dims.height - 1 - y));
      if (edge < 2) {
        const Coord c{static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)};
        height[sz(g.dims.index(c))] -= 0.12f * static_cast<float>(2 - edge);
      }
    }
  }

  int water_target = static_cast<int>(
      static_cast<float>(g.n) * (1.0f - cfg.land_ratio) + 0.5f);
  water_target = std::clamp(water_target, 0, g.n - static_cast<int>(g.capitals.size()) -
                                                  static_cast<int>(g.villages.size()));

  for (int i = 0; i < g.n; ++i) g.st->tiles[i].terrain = Terrain::Field;

  // §6.2: lighthouses sit on the four map corners, and in the real game those are sea. Force
  // the corners to water FIRST and charge them against the quota, so the land ratio stays
  // exact rather than drifting by four tiles.
  std::vector<TileIdx> corners;
  const auto corner = [&](int x, int y) {
    corners.push_back(g.dims.index({static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)}));
  };
  corner(0, 0);
  corner(g.dims.width - 1, 0);
  corner(0, g.dims.height - 1);
  corner(g.dims.width - 1, g.dims.height - 1);
  std::sort(corners.begin(), corners.end());
  corners.erase(std::unique(corners.begin(), corners.end()), corners.end());

  int placed = 0;
  std::vector<bool> is_water(sz(g.n), false);
  if (water_target >= static_cast<int>(corners.size())) {
    for (const TileIdx t : corners) {
      if (forced_land[sz(t)]) continue;
      is_water[sz(t)] = true;
      ++placed;
    }
  }

  std::vector<int> order;
  order.reserve(sz(g.n));
  for (int i = 0; i < g.n; ++i) {
    if (!forced_land[sz(i)] && !is_water[sz(i)]) order.push_back(i);
  }
  std::sort(order.begin(), order.end(), [&](int a, int b) {
    if (height[sz(a)] != height[sz(b)]) return height[sz(a)] < height[sz(b)];
    return a < b;  // index tiebreak: std::sort is not stable, determinism is not optional
  });
  for (const int i : order) {
    if (placed >= water_target) break;
    is_water[sz(i)] = true;
    ++placed;
  }

  for (int i = 0; i < g.n; ++i) {
    if (is_water[sz(i)]) g.st->tiles[i].terrain = Terrain::Ocean;  // reclassified below
  }
}

// Deep water is water that shares no EDGE with land; everything else that is wet is shallow.
void classify_water(GameState& st) {
  const int n = st.dims.tile_count();
  for (int i = 0; i < n; ++i) {
    if (!is_water_terrain(st.tiles[i].terrain)) continue;
    st.tiles[i].terrain =
        mapgen::shares_edge_with_land(st, static_cast<TileIdx>(i)) ? Terrain::Water : Terrain::Ocean;
  }
}

// ---------------------------------------------------------------------------------------
// Step 4: forest and mountain (§10.4)
// ---------------------------------------------------------------------------------------
// Since update 2.0.58 terrain MATCHES the published percentages -- it is a quota, not a
// per-tile roll ("Luxidoor will always have 48% of their biome as field tiles"). So: partition
// the land by nearest capital, compute that tribe's rates, and take the top-K tiles of an
// independent smoothed noise field for each class. Using noise for the ordering rather than a
// shuffle is what makes mountains form ranges and forests form belts.
//
// Multiplier order is load-bearing (§10.4): mountain first, rescaling forest and field
// proportionally; then forest, which by then can only eat into field; field is never modified
// directly, it is the remainder.

void distribute_forest_and_mountain(Gen& g) {
  const std::vector<float> elevation = smoothed_noise(g.st->rng, g.dims, 2);
  const std::vector<float> woods = smoothed_noise(g.st->rng, g.dims, 2);

  const int players = static_cast<int>(g.capitals.size());
  std::vector<std::vector<int>> by_region(sz(players));

  std::vector<bool> forced_field(sz(g.n), false);
  for (const TileIdx t : g.capitals) forced_field[sz(t)] = true;
  for (const TileIdx t : g.villages) forced_field[sz(t)] = true;

  g.region.assign(sz(g.n), 0);
  for (int i = 0; i < g.n; ++i) {
    int best = 0;
    int best_d = 1 << 30;
    for (int p = 0; p < players; ++p) {
      const int d = cheb(g.at(static_cast<TileIdx>(i)), g.at(g.capitals[sz(p)]));
      if (d < best_d) {
        best_d = d;
        best = p;
      }
    }
    g.region[sz(i)] = best;
    if (is_water_terrain(g.st->tiles[i].terrain) || forced_field[sz(i)]) continue;
    by_region[sz(best)].push_back(i);
  }

  for (int p = 0; p < players; ++p) {
    std::vector<int>& tiles = by_region[sz(p)];
    const int count = static_cast<int>(tiles.size());
    if (count == 0) continue;
    const TribeBias& b = bias_for(g.cfg->tribes[p]);

    const float mountain_rate = std::clamp(kBaseMountain * b.mountain, 0.0f, 0.9f);
    const float remainder = 1.0f - mountain_rate;
    const float forest_share = kBaseForest / (kBaseForest + kBaseField);
    const float forest_rate = std::min(remainder * forest_share * b.forest, remainder);

    int n_mountain = static_cast<int>(static_cast<float>(count) * mountain_rate + 0.5f);
    int n_forest = static_cast<int>(static_cast<float>(count) * forest_rate + 0.5f);
    n_mountain = std::clamp(n_mountain, 0, count);
    n_forest = std::clamp(n_forest, 0, count - n_mountain);

    std::sort(tiles.begin(), tiles.end(), [&](int a, int c) {
      if (elevation[sz(a)] != elevation[sz(c)]) return elevation[sz(a)] > elevation[sz(c)];
      return a < c;
    });
    for (int i = 0; i < n_mountain; ++i) g.st->tiles[tiles[sz(i)]].terrain = Terrain::Mountain;

    std::vector<int> rest(tiles.begin() + n_mountain, tiles.end());
    std::sort(rest.begin(), rest.end(), [&](int a, int c) {
      if (woods[sz(a)] != woods[sz(c)]) return woods[sz(a)] > woods[sz(c)];
      return a < c;
    });
    for (int i = 0; i < n_forest; ++i) g.st->tiles[rest[sz(i)]].terrain = Terrain::Forest;
  }
}

// ---------------------------------------------------------------------------------------
// Step 5: cities, territory, units
// ---------------------------------------------------------------------------------------
// resolutions.md R3: a city's 3x3 claim never takes an already-claimed tile. First come,
// first served, which is why borders are ragged and why owner lives per tile.

void found_cities_and_territory(Gen& g) {
  GameState& st = *g.st;
  const int players = static_cast<int>(g.capitals.size());

  for (int p = 0; p < players; ++p) {
    const TileIdx t = g.capitals[sz(p)];
    const auto cid = static_cast<CityId>(p);
    City& c = st.cities[cid];
    c.owner = static_cast<PlayerId>(p);
    c.tile = t;
    c.level = static_cast<std::int8_t>(capital_level(g.cfg->tribes[p]));
    c.population = 0;
    c.unit_count = 1;
    c.add(CityFlag::Capital);
    st.tiles[t].terrain = Terrain::Field;
    st.tiles[t].resource = Resource::None;
  }
  st.city_count = static_cast<std::int16_t>(players);

  for (int p = 0; p < players; ++p) {
    const Coord centre = g.at(g.capitals[sz(p)]);
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        const Coord c{static_cast<std::int16_t>(centre.x + dx),
                      static_cast<std::int16_t>(centre.y + dy)};
        if (!g.dims.contains(c)) continue;
        Tile& tile = st.tiles[g.dims.index(c)];
        if (tile.owner != kNoPlayer) continue;  // R3
        tile.owner = static_cast<PlayerId>(p);
        tile.city = static_cast<CityId>(p);
      }
    }
  }

  for (int p = 0; p < players; ++p) {
    const TileIdx t = g.capitals[sz(p)];
    const int tribe_index = static_cast<int>(g.cfg->tribes[p]);
    const UnitType type =
        kStartingUnit[(tribe_index >= 0 && tribe_index < kTribeCount) ? tribe_index : 0];
    const auto uid = static_cast<UnitId>(p);
    Unit& u = st.units[uid];
    u.type = type;
    u.carried_type = type;
    u.owner = static_cast<PlayerId>(p);
    u.tile = t;
    u.hp = to_hp(starting_unit_hp(type));
    u.home_city = static_cast<CityId>(p);
    u.moves_left = 0;  // the turn machine grants movement at turn start
    st.tiles[t].unit = uid;
  }
  st.unit_count = static_cast<std::int16_t>(players);

  for (int p = 0; p < players; ++p) {
    Player& pl = st.players[p];
    pl.tribe = g.cfg->tribes[p];
    pl.alive = true;
  }
  st.player_count = static_cast<std::int8_t>(players);
}

// ---------------------------------------------------------------------------------------
// Step 6: distance-to-city field
// ---------------------------------------------------------------------------------------
// §6.2's single most restrictive rule: "all resources spawn only within two tiles of cities or
// villages". Everything downstream consults this, so it is computed once.

void compute_city_distance(Gen& g) {
  g.city_dist.assign(sz(g.n), 1 << 20);
  std::vector<TileIdx> seeds = g.capitals;
  seeds.insert(seeds.end(), g.villages.begin(), g.villages.end());
  for (int i = 0; i < g.n; ++i) {
    int best = 1 << 20;
    for (const TileIdx s : seeds) best = std::min(best, cheb(g.at(static_cast<TileIdx>(i)), g.at(s)));
    g.city_dist[sz(i)] = best;
  }
}

// ---------------------------------------------------------------------------------------
// Step 7: lighthouses, then resources
// ---------------------------------------------------------------------------------------

void place_lighthouses(Gen& g) {
  const auto put = [&](int x, int y) {
    const Coord c{static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)};
    if (!g.dims.contains(c)) return;
    Tile& t = g.st->tiles[g.dims.index(c)];
    if (t.resource != Resource::None) return;
    t.resource = Resource::Lighthouse;
  };
  put(0, 0);
  put(g.dims.width - 1, 0);
  put(0, g.dims.height - 1);
  put(g.dims.width - 1, g.dims.height - 1);
}

void place_villages_as_resource(Gen& g) {
  for (const TileIdx t : g.villages) {
    g.st->tiles[t].terrain = Terrain::Field;
    g.st->tiles[t].resource = Resource::Village;
  }
}

void place_resources(Gen& g) {
  GameState& st = *g.st;
  for (int i = 0; i < g.n; ++i) {
    Tile& tile = st.tiles[i];
    if (tile.resource != Resource::None) continue;      // village or lighthouse
    if (g.city_dist[sz(i)] == 0) continue;              // the city tile itself
    if (g.city_dist[sz(i)] > kResourceRange) continue;  // §6.2

    const bool inner = g.city_dist[sz(i)] <= 1;
    const TribeBias& b = bias_for(g.cfg->tribes[g.region[sz(i)]]);

    switch (tile.terrain) {
      case Terrain::Field: {
        float fruit = (inner ? kInnerFruit : kOuterFruit) * b.fruit;
        float crop = (inner ? kInnerCrop : kOuterCrop) * b.crop;
        const float total = fruit + crop;
        if (total > 1.0f) {
          fruit /= total;
          crop /= total;
        }
        const float r = st.rng.unit_float();
        if (r < fruit) {
          tile.resource = Resource::Fruit;
        } else if (r < fruit + crop) {
          tile.resource = Resource::Crop;
        }
        break;
      }
      case Terrain::Forest: {
        const float rate = std::min((inner ? kInnerAnimal : kOuterAnimal) * b.animal, 1.0f);
        if (st.rng.chance(rate)) tile.resource = Resource::Game;
        break;
      }
      case Terrain::Mountain: {
        const float rate = std::min((inner ? kInnerMetal : kOuterMetal) * b.metal, 1.0f);
        if (st.rng.chance(rate)) tile.resource = Resource::Metal;
        break;
      }
      case Terrain::Water: {
        const float rate = std::min(kFishRate * b.fish, 1.0f);
        if (st.rng.chance(rate)) tile.resource = Resource::Fish;
        break;
      }
      case Terrain::Ocean: {
        // §10.4: "Fish can also spawn in ocean inside borders."
        if (tile.owner == kNoPlayer) break;
        const float rate = std::min(kFishRate * b.fish, 1.0f);
        if (st.rng.chance(rate)) tile.resource = Resource::Fish;
        break;
      }
      default:
        break;
    }
  }
}

// §10.4: the four "turn-0" tribes always spawn with enough to upgrade on turn 0, force-spawned
// if world generation did not oblige. Only land terrain is ever converted here -- a forced Fish
// would have to create water, which would silently break the land-ratio quota, so Kickoo simply
// goes without if its capital ring is dry.
void force_capital_resources(Gen& g) {
  GameState& st = *g.st;
  const int players = static_cast<int>(g.capitals.size());
  for (int p = 0; p < players; ++p) {
    const int tribe_index = static_cast<int>(g.cfg->tribes[p]);
    if (tribe_index < 0 || tribe_index >= kTribeCount) continue;
    const Resource want = kGuaranteedResource[tribe_index];
    const Coord centre = g.at(g.capitals[sz(p)]);

    bool present = false;
    std::vector<TileIdx> ring;
    for (const Coord off : kNeighbourOffsets) {
      const Coord c{static_cast<std::int16_t>(centre.x + off.x),
                    static_cast<std::int16_t>(centre.y + off.y)};
      if (!g.dims.contains(c)) continue;
      const TileIdx t = g.dims.index(c);
      if (st.tiles[t].resource == want) present = true;
      ring.push_back(t);
    }
    if (present || ring.empty()) continue;

    const Terrain needed = want == Resource::Metal  ? Terrain::Mountain
                           : want == Resource::Game ? Terrain::Forest
                           : want == Resource::Fish ? Terrain::Water
                                                    : Terrain::Field;

    // Prefer a tile that already has the right terrain and nothing on it; otherwise convert a
    // spare land tile. Never touch water, a village, or another already-placed resource.
    TileIdx chosen = kNoTile;
    for (const TileIdx t : ring) {
      if (st.tiles[t].resource != Resource::None) continue;
      if (st.tiles[t].terrain == needed) {
        chosen = t;
        break;
      }
    }
    if (chosen == kNoTile && needed != Terrain::Water) {
      for (const TileIdx t : ring) {
        if (st.tiles[t].resource != Resource::None) continue;
        if (is_water_terrain(st.tiles[t].terrain)) continue;
        chosen = t;
        st.tiles[t].terrain = needed;
        break;
      }
    }
    if (chosen != kNoTile) st.tiles[chosen].resource = want;
  }
}

// §6.2: starfish spawn on any water tile at roughly 1 per 25 water tiles, never adjacent to
// another starfish, a lighthouse, or a city.
void place_starfish(Gen& g) {
  GameState& st = *g.st;
  std::vector<TileIdx> water;
  for (int i = 0; i < g.n; ++i) {
    if (is_water_terrain(st.tiles[i].terrain)) water.push_back(static_cast<TileIdx>(i));
  }
  const int want = static_cast<int>(static_cast<float>(water.size()) * kStarfishPerWater + 0.5f);
  if (want <= 0) return;

  std::vector<TileIdx> candidates;
  for (const TileIdx t : water) {
    if (st.tiles[t].resource != Resource::None) continue;
    if (g.city_dist[sz(t)] > kResourceRange) continue;
    candidates.push_back(t);
  }
  shuffle(candidates, st.rng);

  int placed = 0;
  for (const TileIdx t : candidates) {
    if (placed >= want) break;
    const Coord c = g.at(t);
    bool ok = true;
    for (const Coord off : kNeighbourOffsets) {
      const Coord nb{static_cast<std::int16_t>(c.x + off.x), static_cast<std::int16_t>(c.y + off.y)};
      if (!g.dims.contains(nb)) continue;
      const Tile& other = st.tiles[g.dims.index(nb)];
      if (other.resource == Resource::Starfish || other.resource == Resource::Lighthouse ||
          other.resource == Resource::Village || g.city_dist[sz(g.dims.index(nb))] == 0) {
        ok = false;
        break;
      }
    }
    if (!ok) continue;
    st.tiles[t].resource = Resource::Starfish;
    ++placed;
  }
}

// §10.5: ruins are placed last, after villages, resources and lighthouses. §6.2/§10.5: never
// adjacent to a capital, never adjacent to another ruin, never on shallow water.
void place_ruins(Gen& g) {
  GameState& st = *g.st;
  const int want = ruin_count_for(g.n);

  std::vector<TileIdx> candidates;
  for (int i = 0; i < g.n; ++i) {
    const Tile& tile = st.tiles[i];
    if (tile.resource != Resource::None) continue;
    if (tile.terrain == Terrain::Water) continue;  // §6.2: any terrain except shallow water
    if (g.city_dist[sz(i)] == 0) continue;
    if (g.city_dist[sz(i)] > kResourceRange) continue;
    candidates.push_back(static_cast<TileIdx>(i));
  }
  shuffle(candidates, st.rng);

  std::vector<bool> is_capital(sz(g.n), false);
  for (const TileIdx t : g.capitals) is_capital[sz(t)] = true;

  int placed = 0;
  for (const TileIdx t : candidates) {
    if (placed >= want) break;
    const Coord c = g.at(t);
    bool ok = true;
    for (const Coord off : kNeighbourOffsets) {
      const Coord nb{static_cast<std::int16_t>(c.x + off.x), static_cast<std::int16_t>(c.y + off.y)};
      if (!g.dims.contains(nb)) continue;
      const TileIdx ni = g.dims.index(nb);
      if (is_capital[sz(ni)] || st.tiles[ni].resource == Resource::Ruin) {
        ok = false;
        break;
      }
    }
    if (!ok) continue;
    st.tiles[t].resource = Resource::Ruin;
    ++placed;
  }
}

// ---------------------------------------------------------------------------------------
// Step 8: initial fog (resolutions.md R18)
// ---------------------------------------------------------------------------------------

void reveal_start(Gen& g) {
  GameState& st = *g.st;
  const int players = static_cast<int>(g.capitals.size());
  for (int p = 0; p < players; ++p) {
    const Coord centre = g.at(g.capitals[sz(p)]);
    for (int dy = -kInitialVisionRadius; dy <= kInitialVisionRadius; ++dy) {
      for (int dx = -kInitialVisionRadius; dx <= kInitialVisionRadius; ++dx) {
        const Coord c{static_cast<std::int16_t>(centre.x + dx),
                      static_cast<std::int16_t>(centre.y + dy)};
        if (g.dims.contains(c)) st.explored[p].set(g.dims.index(c));
      }
    }
  }
}

// ---------------------------------------------------------------------------------------
// Shared config validation
// ---------------------------------------------------------------------------------------

bool validate(const MapConfig& cfg, std::string& error) {
  if (cfg.dims.width < 3 || cfg.dims.height < 3) {
    error = "map dimensions must be at least 3x3";
    return false;
  }
  if (cfg.dims.width > kMaxMapDim || cfg.dims.height > kMaxMapDim) {
    error = "map dimensions exceed kMaxMapDim (" + std::to_string(kMaxMapDim) + ")";
    return false;
  }
  if (cfg.dims.tile_count() > kMaxTiles) {
    error = "map exceeds kMaxTiles";
    return false;
  }
  if (cfg.player_count < 1 || cfg.player_count > kMaxPlayers) {
    error = "player_count must be in [1, " + std::to_string(kMaxPlayers) + "]";
    return false;
  }
  if (cfg.player_count > kMaxCities || cfg.player_count > kMaxUnits) {
    error = "player_count exceeds city or unit capacity";
    return false;
  }
  if (!(cfg.land_ratio >= 0.0f) || !(cfg.land_ratio <= 1.0f)) {
    error = "land_ratio must be in [0, 1]";
    return false;
  }
  for (int p = 0; p < cfg.player_count; ++p) {
    const int t = static_cast<int>(cfg.tribes[p]);
    if (t < 0 || t >= kTribeCount) {
      error = "unknown tribe for player " + std::to_string(p);
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------------------
// Fixed-map parsing helpers
// ---------------------------------------------------------------------------------------

bool tribe_from_name(const std::string& name, Tribe& out) {
  static const struct {
    const char* name;
    Tribe tribe;
  } kNames[] = {
      {"xinxi", Tribe::XinXi},       {"xin-xi", Tribe::XinXi},
      {"imperius", Tribe::Imperius}, {"bardur", Tribe::Bardur},
      {"oumaji", Tribe::Oumaji},     {"kickoo", Tribe::Kickoo},
      {"hoodrick", Tribe::Hoodrick}, {"luxidoor", Tribe::Luxidoor},
      {"vengir", Tribe::Vengir},     {"zebasi", Tribe::Zebasi},
      {"aimo", Tribe::AiMo},         {"ai-mo", Tribe::AiMo},
      {"quetzali", Tribe::Quetzali}, {"yadakk", Tribe::Yadakk},
  };
  std::string lower;
  lower.reserve(name.size());
  for (const char ch : name) {
    lower += (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
  }
  for (const auto& entry : kNames) {
    if (lower == entry.name) {
      out = entry.tribe;
      return true;
    }
  }
  return false;
}

bool terrain_from_char(char c, Terrain& out) {
  switch (c) {
    case '.': case 'g': out = Terrain::Field; return true;
    case 'f': out = Terrain::Forest; return true;
    case 'm': out = Terrain::Mountain; return true;
    case 'w': case 'o': case '~': out = Terrain::Water; return true;  // reclassified later
    default: return false;
  }
}

bool resource_from_char(char c, Resource& out) {
  switch (c) {
    case '.': case ' ': out = Resource::None; return true;
    case 'F': out = Resource::Fruit; return true;
    case 'C': out = Resource::Crop; return true;
    case 'G': out = Resource::Game; return true;
    case 'i': out = Resource::Fish; return true;
    case 'S': out = Resource::Starfish; return true;
    case 'M': out = Resource::Metal; return true;
    case 'R': out = Resource::Ruin; return true;
    case 'V': out = Resource::Village; return true;
    case 'L': out = Resource::Lighthouse; return true;
    default: return false;
  }
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------

namespace mapgen {

bool is_water(Terrain t) { return is_water_terrain(t); }

bool shares_edge_with_land(const GameState& state, TileIdx t) {
  const Coord c = state.dims.coord(t);
  for (const Coord off : kEdgeOffsets) {
    const Coord nb{static_cast<std::int16_t>(c.x + off.x), static_cast<std::int16_t>(c.y + off.y)};
    if (!state.dims.contains(nb)) continue;
    if (!is_water_terrain(state.tiles[state.dims.index(nb)].terrain)) return true;
  }
  return false;
}

}  // namespace mapgen

bool generate_map(const MapConfig& cfg, GameState& state, std::string& error) {
  error.clear();
  if (!validate(cfg, error)) return false;

  // Full reset, INCLUDING the capacity beyond the live map. Two states generated from equal
  // configs must be byte-identical, and stale bytes in the unused tail would break that for
  // any caller that hashes the whole struct.
  state = GameState{};
  state.dims = cfg.dims;
  state.rng.seed(cfg.seed, 1);
  state.turn = 0;
  state.current_player = 0;
  state.phase = Phase::Select;
  state.selected = kNoTile;

  Gen g;
  g.cfg = &cfg;
  g.st = &state;
  g.dims = cfg.dims;
  g.n = cfg.dims.tile_count();

  if (!place_capitals(g, error)) return false;
  place_villages(g);
  split_land_and_water(g);
  classify_water(state);
  distribute_forest_and_mountain(g);
  found_cities_and_territory(g);
  compute_city_distance(g);
  place_villages_as_resource(g);
  place_lighthouses(g);
  place_resources(g);
  force_capital_resources(g);
  place_starfish(g);
  place_ruins(g);
  reveal_start(g);

  // force_capital_resources may have converted a land tile's terrain; nothing it does can turn
  // land into water, but re-running the classifier costs nothing and removes a class of "the
  // fixture edited terrain and forgot" bug for load_fixed_map, which shares this tail.
  classify_water(state);
  return true;
}

bool load_fixed_map(const std::string& path, GameState& state, std::string& error) {
  error.clear();
  json::Value root;
  if (!json::parse_file(path, root, error)) return false;
  if (!root.is_object()) {
    error = "fixed map: root must be an object";
    return false;
  }

  const int width = root["width"].integer(0);
  const int height = root["height"].integer(0);
  MapConfig cfg;
  cfg.dims = MapDims{static_cast<std::int16_t>(width), static_cast<std::int16_t>(height)};
  cfg.seed = static_cast<std::uint64_t>(root["seed"].integer(0));

  const json::Array& players = root["players"].arr();
  cfg.player_count = static_cast<int>(players.size());
  if (!validate(cfg, error)) {
    error = "fixed map: " + error;
    return false;
  }

  const json::Array& terrain_rows = root["terrain"].arr();
  if (static_cast<int>(terrain_rows.size()) != height) {
    error = "fixed map: expected " + std::to_string(height) + " terrain rows";
    return false;
  }
  const json::Array& resource_rows = root["resources"].arr();
  if (!resource_rows.empty() && static_cast<int>(resource_rows.size()) != height) {
    error = "fixed map: expected " + std::to_string(height) + " resource rows";
    return false;
  }

  state = GameState{};
  state.dims = cfg.dims;
  state.rng.seed(cfg.seed, 1);
  state.phase = Phase::Select;
  state.selected = kNoTile;

  for (int y = 0; y < height; ++y) {
    const std::string& row = terrain_rows[sz(y)].str();
    if (static_cast<int>(row.size()) != width) {
      error = "fixed map: terrain row " + std::to_string(y) + " is not " + std::to_string(width) +
              " chars";
      return false;
    }
    for (int x = 0; x < width; ++x) {
      Terrain terrain{};
      if (!terrain_from_char(row[sz(x)], terrain)) {
        error = "fixed map: unknown terrain char at " + std::to_string(x) + "," + std::to_string(y);
        return false;
      }
      state.tiles[cfg.dims.index({static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)})]
          .terrain = terrain;
    }
  }

  for (int y = 0; y < static_cast<int>(resource_rows.size()); ++y) {
    const std::string& row = resource_rows[sz(y)].str();
    if (static_cast<int>(row.size()) != width) {
      error = "fixed map: resource row " + std::to_string(y) + " is not " + std::to_string(width) +
              " chars";
      return false;
    }
    for (int x = 0; x < width; ++x) {
      Resource resource{};
      if (!resource_from_char(row[sz(x)], resource)) {
        error = "fixed map: unknown resource char at " + std::to_string(x) + "," +
                std::to_string(y);
        return false;
      }
      state.tiles[cfg.dims.index({static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)})]
          .resource = resource;
    }
  }

  Gen g;
  g.cfg = &cfg;
  g.st = &state;
  g.dims = cfg.dims;
  g.n = cfg.dims.tile_count();

  for (int p = 0; p < cfg.player_count; ++p) {
    const json::Value& entry = players[sz(p)];
    Tribe tribe{};
    if (!tribe_from_name(entry["tribe"].str(), tribe)) {
      error = "fixed map: unknown tribe for player " + std::to_string(p);
      return false;
    }
    cfg.tribes[p] = tribe;

    const json::Value& capital = entry["capital"];
    TileIdx t = kNoTile;
    if (capital.is_array() && capital.arr().size() == 2) {
      const Coord c{static_cast<std::int16_t>(capital.arr()[0].integer()),
                    static_cast<std::int16_t>(capital.arr()[1].integer())};
      if (!cfg.dims.contains(c)) {
        error = "fixed map: capital out of range for player " + std::to_string(p);
        return false;
      }
      t = cfg.dims.index(c);
    } else if (capital.is_number()) {
      t = static_cast<TileIdx>(capital.integer(-1));
    }
    if (t < 0 || t >= static_cast<TileIdx>(g.n)) {
      error = "fixed map: capital out of range for player " + std::to_string(p);
      return false;
    }
    cfg.start_tiles[p] = t;
    g.capitals.push_back(t);
  }
  cfg.fixed_starts = true;

  for (int i = 0; i < g.n; ++i) {
    if (state.tiles[i].resource == Resource::Village) {
      state.tiles[i].terrain = Terrain::Field;
      g.villages.push_back(static_cast<TileIdx>(i));
    }
  }

  classify_water(state);
  found_cities_and_territory(g);
  compute_city_distance(g);
  // region is only consulted by resource placement, which a fixture supplies itself; fill it
  // anyway so the struct is never half-initialised for a future caller.
  g.region.assign(sz(g.n), 0);
  reveal_start(g);
  return true;
}

}  // namespace poly
