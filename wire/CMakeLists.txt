# The WIRE PROTOCOL layer: engine state -> the JSON the browser consumes.
#
# Its own module rather than part of core or cli, because it is neither. The engine must not
# know a UI exists; the CLI is one transport among several (a WASM build talks to the same
# projection without a socket). Layering is core -> wire -> cli, one direction only.
add_library(poly_wire STATIC state_view.cpp)
target_link_libraries(poly_wire PUBLIC poly)
target_include_directories(poly_wire PUBLIC ${CMAKE_SOURCE_DIR})
