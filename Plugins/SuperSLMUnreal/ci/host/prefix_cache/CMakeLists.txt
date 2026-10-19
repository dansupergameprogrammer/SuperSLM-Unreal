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

set(WARN_FLAGS -Wall -Wextra -Werror -Wshadow)
set(SAN_FLAGS "")
if(PREFIX_CACHE_SANITIZE)
  set(SAN_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
endif()

# Layer 1's SHA-256, the key's hash (precache plan §5.2). It keeps Layer 1's own exception contract
# ("throws only std::bad_alloc"), so it is the one target built with exceptions.
add_library(layer1_sha256 STATIC ${LAYER1_DIR}/src/sha256.cpp)
target_include_directories(layer1_sha256 PUBLIC ${LAYER1_DIR}/include PRIVATE ${LAYER1_DIR}/src)
target_compile_options(layer1_sha256 PRIVATE -Wall -Wextra -Werror ${SAN_FLAGS})

# The cores, built as Unreal will build them: no exceptions, no RTTI, and the warnings MSVC raises
# under Unreal's settings (shadowing, narrowing, undefined macros in #if) made errors here.
add_library(prefix_cache_core STATIC
  ${CORE_DIR}/SuperSLMResidencyCore.cpp
  ${CORE_DIR}/SuperSLMPrefixCacheKey.cpp
  ${CORE_DIR}/SuperSLMPrefixCacheWrapper.cpp
  ${CORE_DIR}/SuperSLMPrefixDiskCache.cpp
  ${CORE_DIR}/SuperSLMPrefixCacheCore.cpp)
target_include_directories(prefix_cache_core PUBLIC ${CORE_DIR})
target_link_libraries(prefix_cache_core PUBLIC layer1_sha256)
target_compile_options(prefix_cache_core PRIVATE ${WARN_FLAGS} -Wconversion -Wsign-conversion -Wundef
  -fno-exceptions -fno-rtti ${SAN_FLAGS})

# The cores as one translation unit (Unreal's unity build): built, never linked.
add_library(prefix_cache_core_unity OBJECT unity_cores.cpp)
target_include_directories(prefix_cache_core_unity PRIVATE ${CORE_DIR} ${LAYER1_DIR}/include)
target_compile_options(prefix_cache_core_unity PRIVATE ${WARN_FLAGS} -Wconversion -Wsign-conversion -Wundef
  -fno-exceptions -fno-rtti)

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
  test_cache_churn.cpp)
target_link_libraries(prefix_cache_host_tests PRIVATE prefix_cache_core)
add_dependencies(prefix_cache_host_tests prefix_cache_core_unity)
target_compile_options(prefix_cache_host_tests PRIVATE ${WARN_FLAGS} -fno-exceptions -fno-rtti ${SAN_FLAGS})
target_link_options(prefix_cache_host_tests PRIVATE ${SAN_FLAGS})

enable_testing()
add_test(NAME prefix_cache_host_tests COMMAND prefix_cache_host_tests)
