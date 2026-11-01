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

# ME_SANITIZE=address  -> AddressSanitizer + UndefinedBehaviorSanitizer
# ME_SANITIZE=thread   -> ThreadSanitizer (data races in the lock-free paths)
set(ME_SANITIZE "" CACHE STRING "address | thread | empty")
if(ME_SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
elseif(ME_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)
include_directories(${CMAKE_SOURCE_DIR}/include)

# ---------------------------------------------------------------- engine lib
add_library(me_core STATIC src/async_logger.cpp src/engine.cpp src/recovery.cpp)
target_link_libraries(me_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------- tools
add_executable(gen_flow tools/gen_flow.cpp)
add_executable(exchange_sim tools/exchange_sim.cpp)
add_executable(l2_rebuild tools/l2_rebuild.cpp)
add_executable(replay_diff tools/replay_diff.cpp)
add_executable(factor_runner tools/factor_runner.cpp)
add_executable(crash_test tools/crash_test.cpp)
target_link_libraries(crash_test PRIVATE me_core)
add_executable(hiccup tools/hiccup.cpp)
target_link_libraries(hiccup PRIVATE Threads::Threads)
target_link_libraries(factor_runner PRIVATE ${CMAKE_DL_LIBS})

# ---------------------------------------------------------------- benches
add_executable(bench_book bench/bench_book.cpp)
target_link_libraries(bench_book PRIVATE me_core)
add_executable(bench_engine bench/bench_engine.cpp)
target_link_libraries(bench_engine PRIVATE me_core)

# ---------------------------------------------------------------- factor plugins (.so)
foreach(fam momentum volatility volume reversion)
  add_library(factor_${fam} SHARED plugins/${fam}.cpp)
  set_target_properties(factor_${fam} PROPERTIES PREFIX "lib" CXX_VISIBILITY_PRESET hidden)
endforeach()

# ---------------------------------------------------------------- tests
add_executable(unit_tests tests/test_main.cpp tests/test_book.cpp tests/test_engine.cpp tests/test_recovery.cpp)
target_link_libraries(unit_tests PRIVATE me_core)
enable_testing()
add_test(NAME unit_tests COMMAND unit_tests)

# ---------------------------------------------------------------- fuzzing (clang only)
option(ME_FUZZ "Build the libFuzzer target (requires clang)" OFF)
if(ME_FUZZ)
  add_executable(fuzz_book fuzz/fuzz_book.cpp)
  target_compile_options(fuzz_book PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(fuzz_book PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
