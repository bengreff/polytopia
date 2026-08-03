add_executable(poly_tests
  test_main.cpp
  test_rng.cpp
  test_geometry.cpp
  test_content.cpp
  test_state.cpp
  test_json.cpp
  test_knowledge.cpp
  test_mechanics.cpp
  test_mapgen.cpp
)

target_include_directories(poly_tests PRIVATE .)
target_link_libraries(poly_tests PRIVATE poly poly_flags)

add_test(NAME poly_tests COMMAND poly_tests)
