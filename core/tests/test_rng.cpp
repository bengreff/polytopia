#include <array>
#include <cstdint>

#include "poly/rng.hpp"
#include "testing.hpp"

using poly::Pcg32;

TEST("rng: same seed reproduces the same stream") {
  Pcg32 a;
  Pcg32 b;
  a.seed(12345);
  b.seed(12345);
  for (int i = 0; i < 1000; ++i) CHECK_EQ(a.next(), b.next());
}

TEST("rng: different seeds diverge") {
  Pcg32 a;
  Pcg32 b;
  a.seed(1);
  b.seed(2);
  int same = 0;
  for (int i = 0; i < 1000; ++i) {
    if (a.next() == b.next()) ++same;
  }
  // Collisions in 32 bits over 1000 draws should be zero in practice.
  CHECK(same < 5);
}

TEST("rng: streams are independent under a shared seed") {
  Pcg32 a;
  Pcg32 b;
  a.seed(7, 1);
  b.seed(7, 2);
  int same = 0;
  for (int i = 0; i < 1000; ++i) {
    if (a.next() == b.next()) ++same;
  }
  CHECK(same < 5);
}

TEST("rng: copying preserves position exactly") {
  // GameState is copied on every search node and carries its RNG by value. If a clone did
  // not resume the identical stream, two searches over the same position would silently
  // diverge, which is the hardest possible class of bug to notice.
  Pcg32 a;
  a.seed(999);
  for (int i = 0; i < 50; ++i) a.next();

  Pcg32 clone = a;
  CHECK(clone == a);
  for (int i = 0; i < 100; ++i) CHECK_EQ(clone.next(), a.next());
}

TEST("rng: bounded() stays in range and covers it") {
  Pcg32 rng;
  rng.seed(4242);
  std::array<int, 6> hits{};
  for (int i = 0; i < 60000; ++i) {
    const std::uint32_t roll = rng.bounded(6);
    CHECK(roll < 6);
    ++hits[roll];
  }
  // A modulo-biased generator would skew the low buckets. Expected 10000 each; allow 10%.
  for (const int count : hits) CHECK(count > 9000 && count < 11000);
}

TEST("rng: bounded(0) is defined") {
  Pcg32 rng;
  rng.seed(1);
  CHECK_EQ(rng.bounded(0), 0u);
}

TEST("rng: unit_float stays in [0, 1)") {
  Pcg32 rng;
  rng.seed(31337);
  for (int i = 0; i < 20000; ++i) {
    const float value = rng.unit_float();
    CHECK(value >= 0.0f && value < 1.0f);
  }
}

TEST("rng: chance() approximates its probability") {
  Pcg32 rng;
  rng.seed(2024);
  int hits = 0;
  for (int i = 0; i < 40000; ++i) {
    if (rng.chance(0.25f)) ++hits;
  }
  CHECK(hits > 9000 && hits < 11000);
}
