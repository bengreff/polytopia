add_executable(poly-cli main.cpp audit.cpp)
target_link_libraries(poly-cli PRIVATE poly poly_agents poly_flags)
