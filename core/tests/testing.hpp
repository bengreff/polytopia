// A ~70-line test framework.
//
// Deliberately not GoogleTest or Catch2. This project's dependency posture is that every
// third-party library is a thing to keep patched, a thing that can drift between the three
// build targets, and a thing that has to build under Emscripten later. A test runner is not
// worth any of that. If this ever needs sharding or fixtures, revisit.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <vector>

namespace poly::testing {

struct TestCase {
  const char* name;
  void (*fn)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

inline int& failure_count() {
  static int failures = 0;
  return failures;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

// Renders a value for a failure message. Numbers go through std::to_string; strings print
// as themselves; anything else degrades to a placeholder rather than failing to compile.
// CHECK_EQ is used on ints, enums, and std::string alike, so it cannot assume numeric.
template <typename T>
std::string describe(const T& value) {
  if constexpr (std::is_same_v<T, std::string>) {
    return "\"" + value + "\"";
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else if constexpr (std::is_arithmetic_v<T>) {
    return std::to_string(value);
  } else {
    return "<value>";
  }
}

inline void report_failure(const char* file, int line, const std::string& what) {
  ++failure_count();
  std::fprintf(stderr, "  FAIL %s:%d\n    %s\n", file, line, what.c_str());
}

inline int run_all() {
  int passed = 0;
  for (const auto& test : registry()) {
    const int before = failure_count();
    test.fn();
    if (failure_count() == before) {
      ++passed;
    } else {
      std::fprintf(stderr, "  in test: %s\n", test.name);
    }
  }
  const auto total = static_cast<int>(registry().size());
  std::printf("%d/%d tests passed\n", passed, total);
  return failure_count() == 0 ? 0 : 1;
}

}  // namespace poly::testing

#define POLY_CONCAT_INNER(a, b) a##b
#define POLY_CONCAT(a, b) POLY_CONCAT_INNER(a, b)

#define TEST(name)                                                               \
  static void POLY_CONCAT(poly_test_fn_, __LINE__)();                            \
  static const ::poly::testing::Registrar POLY_CONCAT(poly_test_reg_, __LINE__)( \
      name, &POLY_CONCAT(poly_test_fn_, __LINE__));                              \
  static void POLY_CONCAT(poly_test_fn_, __LINE__)()

#define CHECK(cond)                                                             \
  do {                                                                          \
    if (!(cond)) ::poly::testing::report_failure(__FILE__, __LINE__, #cond);     \
  } while (0)

#define CHECK_EQ(a, b)                                                            \
  do {                                                                            \
    const auto poly_lhs = (a);                                                    \
    const auto poly_rhs = (b);                                                    \
    if (!(poly_lhs == poly_rhs)) {                                                \
      ::poly::testing::report_failure(                                            \
          __FILE__, __LINE__,                                                     \
          std::string(#a) + " == " + #b + "  (lhs=" +                             \
              ::poly::testing::describe(poly_lhs) + " rhs=" +                     \
              ::poly::testing::describe(poly_rhs) + ")");                         \
    }                                                                             \
  } while (0)
