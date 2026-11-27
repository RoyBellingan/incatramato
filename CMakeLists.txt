cmake_minimum_required(VERSION 3.20)
project(incatramato LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(inc_core
  core/src/terrain.cpp
  core/src/soil.cpp
  core/src/drivetrain.cpp
  core/src/rig.cpp
  core/src/transmission.cpp
  core/src/presets.cpp
  core/src/world.cpp)
target_include_directories(inc_core PUBLIC core/include)
target_compile_options(inc_core PUBLIC -Wall -Wextra -Wpedantic)

add_executable(loop_demo apps/loop_demo.cpp)
target_link_libraries(loop_demo PRIVATE inc_core)

enable_testing()

# The playable prototype needs SDL2 (2.0.18 or newer). The core, demo and tests do not.
find_package(SDL2 2.0.18 QUIET)
if(SDL2_FOUND)
  add_executable(play apps/play.cpp)
  target_link_libraries(play PRIVATE inc_core SDL2::SDL2)
else()
  message(STATUS "SDL2 not found: skipping the 'play' app")
endif()

add_executable(core_tests tests/core_tests.cpp)
target_link_libraries(core_tests PRIVATE inc_core)
add_test(NAME core_tests COMMAND core_tests)
if(TARGET play)
  add_test(NAME play_selftest COMMAND play --selftest ${CMAKE_BINARY_DIR}/selftest_)
endif()
