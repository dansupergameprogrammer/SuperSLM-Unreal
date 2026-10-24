# Host suite for the plugin's page-ledger core (plugin paging plan §3.2-§3.5, §3.9, §7).
# The core is UE-independent C++17; this builds it with the host compiler and runs the suite.
#
#   cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=g++          # or clang++
#   cmake --build build && ./build/page_ledger_tests
#   cmake -S . -B build-san -G Ninja -DPAGE_LEDGER_SANITIZE=ON     # ASan + UBSan
#   cmake -S . -B build-trap -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DPAGE_LEDGER_UBSAN_TRAP=ON
#                                       # UBSan without a runtime (a clang with no compiler-rt)
#
# The core is built with -fno-exceptions -fno-rtti, as it must compile inside Unreal; the tests
# use -fno-rtti too (UBSan's vptr check needs one RTTI setting across the polymorphic seam).
cmake_minimum_required(VERSION 3.16)
project(superslm_page_ledger_host CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PAGE_LEDGER_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(PAGE_LEDGER_UBSAN_TRAP "UndefinedBehaviorSanitizer in trap mode (no sanitizer runtime needed)" OFF)

set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../Source/SuperSLMUnreal/Private/Core)

# MSVC (a standing Windows check, box 2026-10-22): /W4 /WX, and the core and the tests without RTTI or exceptions
# (/GR- /EHs-c-, _HAS_EXCEPTIONS=0 as Unreal builds them; the tests use neither). GNU and clang: unchanged.
if(MSVC)
  if(PAGE_LEDGER_SANITIZE OR PAGE_LEDGER_UBSAN_TRAP)
    message(FATAL_ERROR "PAGE_LEDGER_SANITIZE and PAGE_LEDGER_UBSAN_TRAP are for GNU and clang builds")
  endif()
  string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  string(REPLACE "/GR" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  set(CORE_FLAGS /W4 /WX /GR- /EHs-c-)
  set(TEST_FLAGS /W4 /WX /GR- /EHs-c-)
  set(NOEXC_DEFS _HAS_EXCEPTIONS=0)
else()
  set(WARN -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion)
  set(TEST_WARN -Wall -Wextra -Werror)
  set(CORE_FLAGS ${WARN} -fno-exceptions -fno-rtti)
  set(TEST_FLAGS ${TEST_WARN} -fno-rtti)
  set(NOEXC_DEFS "")
endif()

add_library(page_ledger_core STATIC ${CORE_DIR}/SuperSLMPageLedgerCore.cpp)
target_include_directories(page_ledger_core PUBLIC ${CORE_DIR})
target_compile_definitions(page_ledger_core PUBLIC SUPERSLM_PAGE_LEDGER_TEST_HOOKS=1)
target_compile_options(page_ledger_core PRIVATE ${CORE_FLAGS})
target_compile_definitions(page_ledger_core PRIVATE ${NOEXC_DEFS})

add_executable(page_ledger_tests
  main.cpp
  test_arithmetic.cpp
  test_prefix_adopt.cpp
  test_restore_wait.cpp
  test_lifecycle.cpp
  test_two_cap.cpp)
target_link_libraries(page_ledger_tests PRIVATE page_ledger_core)
target_compile_options(page_ledger_tests PRIVATE ${TEST_FLAGS})
target_compile_definitions(page_ledger_tests PRIVATE ${NOEXC_DEFS})

if(PAGE_LEDGER_SANITIZE)
  foreach(T page_ledger_core page_ledger_tests)
    target_compile_options(${T} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    target_link_options(${T} PRIVATE -fsanitize=address,undefined)
  endforeach()
endif()

if(PAGE_LEDGER_UBSAN_TRAP)
  foreach(T page_ledger_core page_ledger_tests)
    target_compile_options(${T} PRIVATE -fsanitize=undefined -fsanitize-trap=undefined)
    target_link_options(${T} PRIVATE -fsanitize=undefined -fsanitize-trap=undefined)
  endforeach()
endif()

enable_testing()
add_test(NAME page_ledger_tests COMMAND page_ledger_tests)
