add_library(poly_agents STATIC
  eval.cpp
  baselines.cpp
  ladder.cpp
)
target_include_directories(poly_agents PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(poly_agents PUBLIC poly PRIVATE poly_flags)
