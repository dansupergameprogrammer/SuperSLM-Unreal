# Host suite for schema jump-forward's UE-independent policy (schema fast-decode plan rev 1.3.5, §6 P1
# and P1-X): Source/SuperSLMUnreal/Private/Core/SuperSLMSchemaJumpCore.* and the per-C pricing in
# Private/SuperSLMPrefillPlanning.h. Builds with g++ and clang on Linux; no network, no Unreal, no engine.
# See README.md for the commands.
cmake_minimum_required(VERSION 3.16)
project(superslm_schema_jump_host CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SCHEMA_JUMP_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(SCHEMA_JUMP_TSAN "Build with ThreadSanitizer (P1X.8's host half)" OFF)

set(PLUGIN_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(PRIVATE_DIR ${PLUGIN_ROOT}/Source/SuperSLMUnreal/Private)
set(CORE_DIR ${PRIVATE_DIR}/Core)
set(LAYER1_DIR ${PLUGIN_ROOT}/Source/ThirdParty/SuperSLM)

find_package(Threads REQUIRED)

set(WARN_FLAGS -Wall -Wextra -Werror -Wshadow)
set(SAN_FLAGS "")
if(SCHEMA_JUMP_SANITIZE AND SCHEMA_JUMP_TSAN)
  message(FATAL_ERROR "SCHEMA_JUMP_SANITIZE and SCHEMA_JUMP_TSAN cannot be combined")
endif()
if(SCHEMA_JUMP_SANITIZE)
  set(SAN_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
endif()
if(SCHEMA_JUMP_TSAN)
  set(SAN_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
endif()

# The core, built as Unreal will build it: no exceptions, no RTTI, and the warnings MSVC raises under
# Unreal's settings made errors here.
add_library(schema_jump_core STATIC ${CORE_DIR}/SuperSLMSchemaJumpCore.cpp)
target_include_directories(schema_jump_core PUBLIC ${CORE_DIR})
target_compile_options(schema_jump_core PRIVATE ${WARN_FLAGS} -Wconversion -Wsign-conversion -Wundef
  -fno-exceptions -fno-rtti ${SAN_FLAGS})

# Every core in Private/Core as one translation unit (Unreal's unity build): built, never linked. A
# helper defined in two cores under one name is a redefinition here.
add_library(schema_jump_core_unity OBJECT unity_cores.cpp)
target_include_directories(schema_jump_core_unity PRIVATE ${CORE_DIR} ${LAYER1_DIR}/include)
target_compile_options(schema_jump_core_unity PRIVATE ${WARN_FLAGS} -Wconversion -Wsign-conversion -Wundef
  -fno-exceptions -fno-rtti)

# The pricing cells compile the plugin's own SuperSLMPrefillPlanning.h through shim/CoreMinimal.h.
add_library(schema_jump_pricing_cells OBJECT test_pricing.cpp test_pricing_arm.cpp)
target_include_directories(schema_jump_pricing_cells PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shim ${PRIVATE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(schema_jump_pricing_cells PRIVATE ${WARN_FLAGS} -fno-exceptions -fno-rtti ${SAN_FLAGS})

# The red engine-verb stubs, compiled against the vendored engine header (v1.13.1, which lacks the
# verbs) with the switch at its default 0: the declarations copied from SuperSLM f03a5358 must
# compile beside the vendored ABI. Built, never linked (they would shadow nothing on the host).
add_library(schema_jump_abi_stubs OBJECT ${PRIVATE_DIR}/SuperSLMSchemaJumpAbiStubs.cpp)
target_include_directories(schema_jump_abi_stubs PRIVATE ${PRIVATE_DIR} ${LAYER1_DIR}/include)
target_compile_options(schema_jump_abi_stubs PRIVATE ${WARN_FLAGS} -Wconversion -Wsign-conversion -Wundef
  -fno-exceptions -fno-rtti)
# The switch follows the vendored engine, as Build.cs sets it: an engine that declares the jump-forward verbs
# (1.15.0 and later) gets SUPERSLM_WITH_SCHEMA_JUMP_FORWARD=1, so SuperSLMSchemaJumpAbi.h only includes its header
# and the stubs compile out; an older engine keeps the default 0 and the copied declarations beside it.
file(READ ${LAYER1_DIR}/include/superslm/sslm_abi_functions.inc SCHEMA_JUMP_LAYER1_VERBS)
string(FIND "${SCHEMA_JUMP_LAYER1_VERBS}" "sslm_seq_jump_forward" SCHEMA_JUMP_VERB_AT)
if(NOT SCHEMA_JUMP_VERB_AT EQUAL -1)
  target_compile_definitions(schema_jump_abi_stubs PRIVATE SUPERSLM_WITH_SCHEMA_JUMP_FORWARD=1)
endif()

add_executable(schema_jump_host_tests
  harness.cpp
  test_setting.cpp
  test_draft_table.cpp
  test_learning.cpp
  test_store.cpp
  test_span_loop.cpp
  test_review_round1.cpp
  $<TARGET_OBJECTS:schema_jump_pricing_cells>)
target_link_libraries(schema_jump_host_tests PRIVATE schema_jump_core Threads::Threads)
# The B7 fixtures (fixtures/d2-table.txt, fixtures/b4a-b2-walked.txt), read by fixture.h's LoadB7.
target_compile_definitions(schema_jump_host_tests PRIVATE SCHEMA_JUMP_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_dependencies(schema_jump_host_tests schema_jump_core_unity schema_jump_abi_stubs)
target_compile_options(schema_jump_host_tests PRIVATE ${WARN_FLAGS} -fno-exceptions -fno-rtti ${SAN_FLAGS})
target_link_options(schema_jump_host_tests PRIVATE ${SAN_FLAGS})

enable_testing()
add_test(NAME schema_jump_host_tests COMMAND schema_jump_host_tests)
