#include <cstring>

#include "poly/action.hpp"
#include "poly/state.hpp"
#include "testing.hpp"

using namespace poly;

TEST("state: is memcpy-cloneable and within budget") {
  // The constraint the whole forward model rests on. The static_asserts in state.hpp fail
  // the build; this reports the actual size so a regression is visible in test output.
  CHECK(std::is_trivially_copyable_v<GameState>);
  CHECK(sizeof(GameState) <= 64 * 1024);
  std::printf("  [info] sizeof(GameState) = %zu bytes\n", sizeof(GameState));
}

TEST("state: a memcpy clone compares equal and diverges independently") {
  GameState a{};
  a.dims = MapDims{11, 11};
  a.rng.seed(777);
  a.player_count = 2;
  a.cities[0].population = 5;
  a.city_count = 1;

  GameState b;
  std::memcpy(&b, &a, sizeof(GameState));

  CHECK_EQ(b.cities[0].population, 5);
  CHECK(b.dims == a.dims);
  CHECK(b.rng == a.rng);

  b.cities[0].population = 9;
  CHECK_EQ(a.cities[0].population, 5);
}

TEST("state: population is signed and survives going deeply negative") {
  // resolutions.md R17. A captured capital loses its whole trade network at once. If this
  // field were unsigned it would underflow to a huge positive on the exact turn a capital
  // is taken -- the single most important moment of a Might game.
  City c{};
  c.level = 4;
  c.population = 3;
  c.population = static_cast<std::int16_t>(c.population - 12);
  CHECK_EQ(c.population, -9);
  CHECK(c.population < 0);
  // The level does NOT move.
  CHECK_EQ(c.level, 4);
  CHECK_EQ(c.unit_capacity(), 5);
}

TEST("state: HP scaling represents half points exactly") {
  // resolutions.md R13, sidestepping the splash-rounding contradiction in UNRESOLVED #1.
  CHECK_EQ(to_hp(10), 20);
  CHECK_EQ(hp_whole(to_hp(10)), 10);
  // A Warrior at 10 HP taking 4.5 splash lands on 5.5, representable exactly.
  const std::int16_t after = static_cast<std::int16_t>(to_hp(10) - 9);
  CHECK_EQ(after, 11);
  CHECK_EQ(hp_whole(after), 5);  // truncating display; the .5 is still in the state
}

TEST("state: fog is a single explored bitset with no stale state") {
  // resolutions.md R18. Revealed tiles never re-fog, so there is no separate
  // currently-visible set and no remembered enemy positions to age out.
  ExploredMap m{};
  CHECK(!m.test(0));
  CHECK(!m.test(1023));
  m.set(0);
  m.set(1023);
  m.set(500);
  CHECK(m.test(0));
  CHECK(m.test(1023));
  CHECK(m.test(500));
  CHECK(!m.test(499));
  CHECK(!m.test(501));
}

TEST("state: tech mask round-trips every tech independently") {
  Player p{};
  CHECK_EQ(p.tech_count(), 0);
  for (int i = 0; i < kTechCount; ++i) {
    const auto t = static_cast<Tech>(i);
    CHECK(!p.has_tech(t));
    p.grant_tech(t);
    CHECK(p.has_tech(t));
  }
  CHECK_EQ(p.tech_count(), kTechCount);
}

TEST("action: the 11x11 head is close to tribes-rl's independently derived 197") {
  // The concrete form of README §1.1: measured per DECISION rather than per turn, this game
  // has a smaller action space than Go (~250). tribes-rl reached Discrete(197) independently.
  const int head = action_head_size(MapDims{11, 11});
  std::printf("  [info] 11x11 action head = %d slots (tribes-rl: 197)\n", head);
  CHECK(head > 150 && head < 250);
  CHECK_EQ(head, 121 + kNonTileSlots);
}

TEST("action: slot classes are disjoint and decode to what they encode") {
  // The encoding's whole safety property: no index is ever ambiguous between two classes.
  for (int i = 0; i < kUnitTypeCount; ++i) {
    const auto t = static_cast<UnitType>(i);
    const int slot = unit_type_to_slot(t);
    CHECK(slot_is_train(slot));
    CHECK(!slot_is_tile(slot));
    CHECK(!slot_is_research(slot));
    CHECK(!slot_is_build(slot));
    CHECK(!slot_is_reward(slot));
    CHECK(slot_to_unit_type(slot) == t);
  }
  for (int i = 0; i < kTechCount; ++i) {
    const auto t = static_cast<Tech>(i);
    const int slot = tech_to_slot(t);
    CHECK(slot_is_research(slot));
    CHECK(!slot_is_train(slot));
    CHECK(slot_to_tech(slot) == t);
  }
  for (int i = 0; i < static_cast<int>(Improvement::Count); ++i) {
    const auto imp = static_cast<Improvement>(i);
    const int slot = improvement_to_slot(imp);
    CHECK(slot_is_build(slot));
    CHECK(!slot_is_research(slot));
    CHECK(slot_to_improvement(slot) == imp);
  }
  for (int i = 0; i < static_cast<int>(CityReward::Count); ++i) {
    const auto r = static_cast<CityReward>(i);
    const int slot = reward_to_slot(r);
    CHECK(slot_is_reward(slot));
    CHECK(!slot_is_build(slot));
    CHECK(slot_to_reward(slot) == r);
  }
}

TEST("action: tile slots decode identically regardless of map size") {
  // Why typed targets are packed ABOVE the full tile capacity rather than above the live
  // tile count: an action index must mean the same thing on an 11x11 and a 32x32 map, so
  // replays and trained policies stay portable across map sizes.
  for (const TileIdx t : {TileIdx{0}, TileIdx{120}, TileIdx{1023}}) {
    CHECK(slot_is_tile(tile_to_slot(t)));
    CHECK_EQ(slot_to_tile(tile_to_slot(t)), t);
  }
  CHECK_EQ(unit_type_to_slot(UnitType::Warrior), kMaxTiles);
}

TEST("action: mask sets, counts, and enumerates") {
  ActionMask mask;
  mask.clear();
  CHECK(mask.empty());
  CHECK_EQ(mask.count(), 0);

  mask.set(3);
  mask.set(70);
  mask.set(3);  // idempotent -- must not double-count
  CHECK_EQ(mask.count(), 2);
  CHECK(mask.test(3));
  CHECK(mask.test(70));
  CHECK(!mask.test(4));

  std::int16_t out[8]{};
  const int written = mask.collect(out, 8);
  CHECK_EQ(written, 2);
  CHECK_EQ(out[0], 3);
  CHECK_EQ(out[1], 70);
}

TEST("action: collect respects its capacity") {
  ActionMask mask;
  mask.clear();
  for (int i = 0; i < 50; ++i) mask.set(i * 3);
  CHECK_EQ(mask.count(), 50);

  std::int16_t out[10]{};
  CHECK_EQ(mask.collect(out, 10), 10);
  CHECK_EQ(out[0], 0);
  CHECK_EQ(out[9], 27);
}

TEST("state: the phase cursor starts at Select") {
  GameState s{};
  CHECK(s.phase == Phase::Select);
  CHECK_EQ(s.selected, kNoTile);
  CHECK(!s.global_actor);
  CHECK_EQ(s.current_player, 0);
}
