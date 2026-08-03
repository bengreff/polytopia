add_library(poly STATIC
  src/version.cpp
  src/json.cpp
  src/rules.cpp
  src/game.cpp
  src/game_setup.cpp
  src/mapgen.cpp
)

target_include_directories(poly PUBLIC include)
target_link_libraries(poly PRIVATE poly_flags)
set_target_properties(poly PROPERTIES POSITION_INDEPENDENT_CODE ON)
