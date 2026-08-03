#include <cstdio>

#include "poly/version.hpp"
#include "testing.hpp"

int main() {
  std::printf("polytopia engine %s\n", poly::engine_version());
  return poly::testing::run_all();
}
