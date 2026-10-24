# Host suite for the prefix cache's UE-independent cores (Source/SuperSLMUnreal/Private/Core).
# Builds with g++ and clang on Linux; no network, no Unreal. See README.md for the commands.
cmake_minimum_required(VERSION 3.16)
project(superslm_prefix_cache_host CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PREFIX_CACHE_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(PLUGIN_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(CORE_DIR ${PLUGIN_ROOT}/Source/SuperSLMUnreal/Private/Core)
set(LAYER1_DIR ${PLUGIN_ROOT}/Source/ThirdParty/SuperSLM)

# Flags per compiler. GNU and clang: unchanged. MSVC (a standing Windows check, box 2026-10-22): /W4 /WX for every
# target, and the cores and the suite without RTTI or exceptions (/GR- /EHs-c-, with _HAS_EXCEPTIONS=0 so the STL
# headers carry no handlers, as Unreal builds them). GNU's -Wconversion/-Wsign-conversion/-Wundef have no exact MSVC
# set; /W4 carries its conversion warnings (C4244, C4267, C4389).
if(MSVC)
  if(PREFIX_CACHE_SANITIZE)
    message(FATAL_ERROR "PREFIX_CACHE_SANITIZE is for GNU and clang builds")
  endif()
  string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  string(REPLACE "/GR" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  set(WARN_FLAGS /W4 /WX)
  set(SHA_FLAGS /W4 /WX /EHsc)
  set(CORE_FLAGS /W4 /WX /GR- /EHs-c-)
  set(TEST_FLAGS /W4 /WX /GR- /EHs-c-)
  set(NOEXC_DEFS _HAS_EXCEPTIONS=0)
else()
  set(WARN_FLAGS -Wall -Wextra -Werror -Wshadow)
  set(SHA_FLAGS -Wall -Wextra -Werror)
  set(CORE_FLAGS ${WARN_FLAGS} -Wconversion -Wsign-conversion -Wundef -fno-exceptions -fno-rtti)
  set(TEST_FLAGS ${WARN_FLAGS} -fno-exceptions -fno-rtti)
  set(NOEXC_DEFS "")
endif()
set(SAN_FLAGS "")
if(PREFIX_CACHE_SANITIZE)
  set(SAN_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
endif()

# Layer 1's SHA-256, the key's hash (precache plan §5.2). It keeps Layer 1's own exception contract
# ("throws only std::bad_alloc"), so it is the one target built with exceptions.
add_library(layer1_sha256 STATIC ${LAYER1_DIR}/src/sha256.cpp)
target_include_directories(layer1_sha256 PUBLIC ${LAYER1_DIR}/include PRIVATE ${LAYER1_DIR}/src)
target_compile_options(layer1_sha256 PRIVATE ${SHA_FLAGS} ${SAN_FLAGS})

# The cores, built as Unreal will build them: no exceptions, no RTTI, and the warnings MSVC raises
# under Unreal's settings (shadowing, narrowing, undefined macros in #if) made errors here.
add_library(prefix_cache_core STATIC
  ${CORE_DIR}/SuperSLMResidencyCore.cpp
  ${CORE_DIR}/SuperSLMPrefixCacheKey.cpp
  ${CORE_DIR}/SuperSLMPrefixCacheWrapper.cpp
  ${CORE_DIR}/SuperSLMPrefixDiskCache.cpp
  ${CORE_DIR}/SuperSLMPrefixCacheCore.cpp
  ${CORE_DIR}/SuperSLMSaveCompatCore.cpp
  ${CORE_DIR}/SuperSLMPrefixSetCore.cpp
  ${CORE_DIR}/SuperSLMPageLedgerCore.cpp)
target_include_directories(prefix_cache_core PUBLIC ${CORE_DIR})
target_link_libraries(prefix_cache_core PUBLIC layer1_sha256)
target_compile_options(prefix_cache_core PRIVATE ${CORE_FLAGS} ${SAN_FLAGS})
target_compile_definitions(prefix_cache_core PRIVATE ${NOEXC_DEFS})

# The cores as one translation unit (Unreal's unity build): built, never linked.
add_library(prefix_cache_core_unity OBJECT unity_cores.cpp)
target_include_directories(prefix_cache_core_unity PRIVATE ${CORE_DIR} ${LAYER1_DIR}/include)
target_compile_options(prefix_cache_core_unity PRIVATE ${CORE_FLAGS})
target_compile_definitions(prefix_cache_core_unity PRIVATE ${NOEXC_DEFS})

add_executable(prefix_cache_host_tests
  harness.cpp
  sim.cpp
  test_key.cpp
  test_wrapper.cpp
  test_disk.cpp
  test_residency.cpp
  test_cache_charge.cpp
  test_cache_tiers.cpp
  test_cache_dispositions.cpp
  test_cache_lifecycle.cpp
  test_cache_churn.cpp
  test_residency_refund.cpp
  test_cache_refund.cpp
  test_save_compat.cpp
  test_refund_oracle.cpp
  test_prefix_set.cpp
  test_prefix_set_text.cpp
  test_residency_nested.cpp
  test_cache_nested.cpp
  test_sim_seams.cpp
  test_release_status.cpp
  test_residency_control.cpp
  test_residency_order.cpp
  test_residency_disk.cpp
  test_u2_review.cpp
  test_box_regressions.cpp
  test_restore_pricing.cpp
  test_build_slicing.cpp
  test_cache_chunk_pricing.cpp
  test_insitu_prefix_prefill.cpp)
# The executed model's trace (precache plan §5.11 (a); see data/README.md).
target_compile_definitions(prefix_cache_host_tests PRIVATE
  REFUND_ORACLE_TRACE="${CMAKE_CURRENT_SOURCE_DIR}/data/refund_oracle_trace.txt"
  NESTED_REFUND_ORACLE_TRACE="${CMAKE_CURRENT_SOURCE_DIR}/data/nested_refund_oracle_trace.txt")
target_link_libraries(prefix_cache_host_tests PRIVATE prefix_cache_core)
add_dependencies(prefix_cache_host_tests prefix_cache_core_unity)
target_compile_options(prefix_cache_host_tests PRIVATE ${TEST_FLAGS} ${SAN_FLAGS})
target_compile_definitions(prefix_cache_host_tests PRIVATE ${NOEXC_DEFS})
target_link_options(prefix_cache_host_tests PRIVATE ${SAN_FLAGS})

# Cell 11.7's host leg (precache plan §7 item 11.7; suite audit G7): the cores and the suite again with
# asserts compiled out (NDEBUG, the shipping configuration's reading), so the drop's pin check (7.18)
# and its disk-delete serialization (3.10) are graded as real branches, not asserts.
add_library(prefix_cache_core_ndebug STATIC
  ${CORE_DIR}/SuperSLMResidencyCore.cpp
  ${CORE_DIR}/SuperSLMPrefixCacheKey.cpp
  ${CORE_DIR}/SuperSLMPrefixCacheWrapper.cpp
  ${CORE_DIR}/SuperSLMPrefixDiskCache.cpp
  ${CORE_DIR}/SuperSLMPrefixCacheCore.cpp
  ${CORE_DIR}/SuperSLMSaveCompatCore.cpp
  ${CORE_DIR}/SuperSLMPrefixSetCore.cpp
  ${CORE_DIR}/SuperSLMPageLedgerCore.cpp)
target_include_directories(prefix_cache_core_ndebug PUBLIC ${CORE_DIR})
target_link_libraries(prefix_cache_core_ndebug PUBLIC layer1_sha256)
target_compile_definitions(prefix_cache_core_ndebug PRIVATE NDEBUG)
target_compile_options(prefix_cache_core_ndebug PRIVATE ${CORE_FLAGS} ${SAN_FLAGS})
target_compile_definitions(prefix_cache_core_ndebug PRIVATE ${NOEXC_DEFS})
get_target_property(HOST_TEST_SOURCES prefix_cache_host_tests SOURCES)
add_executable(prefix_cache_host_tests_ndebug ${HOST_TEST_SOURCES})
target_compile_definitions(prefix_cache_host_tests_ndebug PRIVATE NDEBUG
  REFUND_ORACLE_TRACE="${CMAKE_CURRENT_SOURCE_DIR}/data/refund_oracle_trace.txt"
  NESTED_REFUND_ORACLE_TRACE="${CMAKE_CURRENT_SOURCE_DIR}/data/nested_refund_oracle_trace.txt")
target_link_libraries(prefix_cache_host_tests_ndebug PRIVATE prefix_cache_core_ndebug)
target_compile_options(prefix_cache_host_tests_ndebug PRIVATE ${TEST_FLAGS} ${SAN_FLAGS})
target_compile_definitions(prefix_cache_host_tests_ndebug PRIVATE ${NOEXC_DEFS})
target_link_options(prefix_cache_host_tests_ndebug PRIVATE ${SAN_FLAGS})

enable_testing()
add_test(NAME prefix_cache_host_tests COMMAND prefix_cache_host_tests)
add_test(NAME prefix_cache_11_7_pin_check COMMAND prefix_cache_host_tests_ndebug residency_drop_of_a_leased_entry)
add_test(NAME prefix_cache_11_7_delete_serialization COMMAND prefix_cache_host_tests_ndebug disk_drop_behind_a)
