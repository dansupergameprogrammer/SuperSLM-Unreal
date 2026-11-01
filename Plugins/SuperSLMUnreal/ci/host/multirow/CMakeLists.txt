# Host suite for the multi-row GPU chooser, plugin 1.4 on engine 1.18 (multi-row GPU plan, MR6; red-suite-mr6.md):
# Source/SuperSLMUnreal/Private/Core/SuperSLMGpuRowsChooser.h. Builds with g++ and clang on Linux; no network, no
# Unreal, no engine. See README.md for the commands.
cmake_minimum_required(VERSION 3.16)
project(superslm_multirow_host CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PLUGIN_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(CORE_DIR ${PLUGIN_ROOT}/Source/SuperSLMUnreal/Private/Core)

set(WARN_FLAGS -Wall -Wextra -Werror -Wshadow)

# The chooser header on its own, under the cores' strict flags (as schema_jump builds the cores). Built, never linked.
add_library(multirow_chooser_strict OBJECT chooser_tu.cpp)
target_include_directories(multirow_chooser_strict PRIVATE ${CORE_DIR})
target_compile_options(multirow_chooser_strict PRIVATE ${WARN_FLAGS} -Wconversion -Wsign-conversion -Wundef -fno-exceptions -fno-rtti)

# Erratum E-13 (ruling 48): the calibration header on its own, under the same strict flags. Built, never linked.
add_library(multirow_calibration_strict OBJECT calibration_tu.cpp)
target_include_directories(multirow_calibration_strict PRIVATE ${CORE_DIR})
target_compile_options(multirow_calibration_strict PRIVATE ${WARN_FLAGS} -Wconversion -Wsign-conversion -Wundef -fno-exceptions -fno-rtti)

add_executable(multirow_host_tests harness.cpp grid.cpp test_chooser.cpp test_calibration.cpp)
target_include_directories(multirow_host_tests PRIVATE ${CORE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
# 10.6's grid, written by chooser_ref.py (python3 chooser_ref.py check grid.txt keeps the two equal).
target_compile_definitions(multirow_host_tests PRIVATE GRID_PATH="${CMAKE_CURRENT_SOURCE_DIR}/grid.txt")
add_dependencies(multirow_host_tests multirow_chooser_strict multirow_calibration_strict)
target_compile_options(multirow_host_tests PRIVATE ${WARN_FLAGS} -fno-exceptions -fno-rtti)

enable_testing()
add_test(NAME multirow_host_tests COMMAND multirow_host_tests)
add_test(NAME multirow_grid_reference COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/chooser_ref.py check ${CMAKE_CURRENT_SOURCE_DIR}/grid.txt)
