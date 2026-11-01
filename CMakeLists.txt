cmake_minimum_required(VERSION 3.16)
project(matching_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
option(ME_NATIVE "Build with -march=native" ON)
if(ME_NATIVE)
  add_compile_options(-march=native)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(Threads REQUIRED)
include_directories(${CMAKE_SOURCE_DIR}/include)

# ---------------------------------------------------------------- engine lib
add_library(me_core STATIC src/async_logger.cpp src/engine.cpp)
target_link_libraries(me_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------- tools
add_executable(gen_flow tools/gen_flow.cpp)
add_executable(exchange_sim tools/exchange_sim.cpp)
add_executable(l2_rebuild tools/l2_rebuild.cpp)
add_executable(replay_diff tools/replay_diff.cpp)
add_executable(factor_runner tools/factor_runner.cpp)
add_executable(hiccup tools/hiccup.cpp)
target_link_libraries(hiccup PRIVATE Threads::Threads)
target_link_libraries(factor_runner PRIVATE ${CMAKE_DL_LIBS})

# ---------------------------------------------------------------- benches
add_executable(bench_book bench/bench_book.cpp)
add_executable(bench_engine bench/bench_engine.cpp)
target_link_libraries(bench_engine PRIVATE me_core)

# ---------------------------------------------------------------- factor plugins (.so)
foreach(fam momentum volatility volume reversion)
  add_library(factor_${fam} SHARED plugins/${fam}.cpp)
  set_target_properties(factor_${fam} PROPERTIES PREFIX "lib" CXX_VISIBILITY_PRESET hidden)
endforeach()

# ---------------------------------------------------------------- tests
add_executable(unit_tests tests/test_main.cpp tests/test_book.cpp tests/test_engine.cpp)
target_link_libraries(unit_tests PRIVATE me_core)
enable_testing()
add_test(NAME unit_tests COMMAND unit_tests)
