add_executable(apicli main.cpp)
target_link_libraries(apicli PRIVATE core)

# --- Core stress driver (STRESS_TEST.md §4/§9) — dev only, do NOT ship in release. ---
option(DEED_BUILD_STRESS "Build the headless Core stress driver (deed_stress)" OFF)
if(DEED_BUILD_STRESS)
  add_executable(deed_stress stress_main.cpp)
  # Need core/src on the include path to reach the internal thread_pool.hpp (header-only) in stress.
  target_include_directories(deed_stress PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
  target_link_libraries(deed_stress PRIVATE core)
endif()
