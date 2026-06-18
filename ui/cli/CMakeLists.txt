add_executable(apicli main.cpp)
target_link_libraries(apicli PRIVATE core)

# --- Stress driver Core (STRESS_TEST.md §4/§9) — dev only, KHÔNG ship release. ---
option(DEED_BUILD_STRESS "Build the headless Core stress driver (deed_stress)" OFF)
if(DEED_BUILD_STRESS)
  add_executable(deed_stress stress_main.cpp)
  # Cần include core/src để chạm thread_pool.hpp nội bộ (header-only) trong stress.
  target_include_directories(deed_stress PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
  target_link_libraries(deed_stress PRIVATE core)
endif()
