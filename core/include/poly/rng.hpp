// PCG32 — the engine's only source of randomness.
//
// This lives inside GameState by value, not beside it. Determinism is a Phase 0
// non-negotiable (docs/phase0-plan.md §1.4): the same seed plus the same action sequence
// must produce a bit-identical state. That property is what makes replays, the golden-trace
// harness, and reproducible bug reports possible, and it is very expensive to retrofit,
// so no code anywhere may reach for a global generator or the clock.
//
// PCG32 rather than std::mt19937 because it is 8 bytes of state instead of ~2.5KB, and
// GameState is copied on every search node.

#pragma once

#include <cstdint>
#include <type_traits>

namespace poly {

struct Pcg32 {
  std::uint64_t state{0x853c49e6748fea9bULL};
  std::uint64_t inc{0xda3e39cb94b95bdbULL};

  // The stream selector lets two logically independent random processes (map generation
  // and in-game rolls, say) share a seed without correlating.
  constexpr void seed(std::uint64_t seed_value, std::uint64_t stream = 1) {
    state = 0;
    inc = (stream << 1u) | 1u;
    next();
    state += seed_value;
    next();
  }

  constexpr std::uint32_t next() {
    const std::uint64_t old = state;
    state = old * 6364136223846793005ULL + inc;
    const auto xorshifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
    const auto rot = static_cast<std::uint32_t>(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
  }

  // Debiased bounded draw (Lemire rejection). A plain modulo would skew low values, which
  // matters here because several rules — ruins outcomes, map resource placement — are
  // small-bound draws where a few percent of bias is a real rules divergence.
  constexpr std::uint32_t bounded(std::uint32_t bound) {
    if (bound == 0) return 0;
    const std::uint32_t threshold = (~bound + 1u) % bound;
    for (;;) {
      const std::uint32_t r = next();
      if (r >= threshold) return r % bound;
    }
  }

  // Uniform in [0, 1). Used for probability comparisons against JSON-configured rates.
  constexpr float unit_float() {
    return static_cast<float>(next() >> 8u) * (1.0f / 16777216.0f);
  }

  [[nodiscard]] constexpr bool chance(float probability) {
    return unit_float() < probability;
  }

  friend constexpr bool operator==(Pcg32, Pcg32) = default;
};

static_assert(std::is_trivially_copyable_v<Pcg32>);
static_assert(sizeof(Pcg32) == 16);

}  // namespace poly
