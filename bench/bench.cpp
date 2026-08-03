// Throughput harness.
//
// This exists at M0, before there is anything meaningful to measure, on purpose.
// docs/phase0-plan.md §8: tracking steps/sec from the first commit means a regression is
// attributable to one commit, instead of being discovered at Phase 4 when the cause is
// buried under months of work. tribes-rl's published 1.8M agent-steps/sec is the reference.
//
// M1 replaces the placeholder workload below with real forward-model steps.

#include <chrono>
#include <cstdint>
#include <cstdio>

#include "poly/rng.hpp"
#include "poly/version.hpp"

namespace {

// Force a value to be treated as observable so the optimiser cannot delete the loop that
// produced it. Without this, clang proves the whole workload has no effect and the harness
// reports physically impossible throughput -- which it did, on the first run.
template <typename T>
void do_not_optimise(T& value) {
  asm volatile("" : "+r,m"(value) : : "memory");
}

template <typename Fn>
double measure_ops_per_sec(std::int64_t iterations, Fn&& fn) {
  const auto start = std::chrono::steady_clock::now();
  fn(iterations);
  const auto end = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(end - start).count();
  return static_cast<double>(iterations) / seconds;
}

}  // namespace

int main() {
  std::printf("polytopia engine %s -- benchmark\n\n", poly::engine_version());

  poly::Pcg32 rng;
  rng.seed(0xC0FFEE);
  std::uint64_t sink = 0;

  const double draws_per_sec = measure_ops_per_sec(50'000'000, [&](std::int64_t n) {
    for (std::int64_t i = 0; i < n; ++i) {
      sink += rng.next();
      do_not_optimise(sink);
    }
  });

  std::printf("  rng.next()          %8.1f M draws/sec\n", draws_per_sec / 1e6);

  const double bounded_per_sec = measure_ops_per_sec(50'000'000, [&](std::int64_t n) {
    for (std::int64_t i = 0; i < n; ++i) {
      sink += rng.bounded(6);
      do_not_optimise(sink);
    }
  });

  std::printf("  rng.bounded(6)      %8.1f M draws/sec\n", bounded_per_sec / 1e6);

  std::printf("\n  PLACEHOLDER: no forward model yet. M1 reports real steps/sec here.\n");
  std::printf("  Reference target: tribes-rl, 1.8M agent-steps/sec.\n");

  // Printed rather than discarded: a nonzero checksum is the proof the work actually ran.
  std::printf("  checksum            %llu\n", static_cast<unsigned long long>(sink));
  return 0;
}
