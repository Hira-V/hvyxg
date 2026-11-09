# Copyright (C) 2026 Kevin H. Patterson
# SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
cmake_minimum_required(VERSION 3.24)
project(hvyxg VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(HVYXG_BUILD_TESTS "Build the doctest test suite" ON)
option(HVYXG_BUILD_TOOLS "Build render_yxg and hvyxg-bench" ON)
option(HVYXG_WERROR "Treat warnings as errors (development builds)" OFF)
set(HVYXG_SANITIZE "" CACHE STRING
    "Build everything with sanitizers: ON = address + undefined, or a -fsanitize= list (e.g. thread)")
option(HVYXG_BUILD_FUZZERS "Build the fuzz targets in fuzz/ (implies HVYXG_SANITIZE; see fuzz/README.md)" OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  set(HVYXG_WARNINGS -Wall -Wextra -Wpedantic -Wshadow)
  if(HVYXG_WERROR)
    list(APPEND HVYXG_WARNINGS -Werror)
  endif()
elseif(MSVC)
  set(HVYXG_WARNINGS /W4)
  if(HVYXG_WERROR)
    list(APPEND HVYXG_WARNINGS /WX)
  endif()
endif()

# Sanitizers (and libFuzzer coverage) apply to every target, the library included.
set(HVYXG_LIBFUZZER OFF)
if(HVYXG_BUILD_FUZZERS AND NOT HVYXG_SANITIZE)
  set(HVYXG_SANITIZE ON)
endif()
if(HVYXG_SANITIZE)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "HVYXG_SANITIZE / HVYXG_BUILD_FUZZERS need Clang or GCC")
  endif()
  set(HVYXG_SANITIZERS ${HVYXG_SANITIZE})
  if(HVYXG_SANITIZE STREQUAL "ON" OR HVYXG_SANITIZE STREQUAL "1" OR HVYXG_SANITIZE STREQUAL "TRUE")
    set(HVYXG_SANITIZERS address,undefined)
  endif()
  add_compile_options(-fsanitize=${HVYXG_SANITIZERS} -fno-omit-frame-pointer -g)
  if(HVYXG_SANITIZERS MATCHES "undefined")
    add_compile_options(-fno-sanitize-recover=undefined)
  endif()
  add_link_options(-fsanitize=${HVYXG_SANITIZERS})
endif()
if(HVYXG_BUILD_FUZZERS)
  # libFuzzer when the compiler has it (LLVM clang; not Apple clang or GCC), else a standalone
  # driver that replays a corpus and mutates it (fuzz/standalone_main.cpp).
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS -fsanitize=fuzzer)
  set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=fuzzer)
  check_cxx_source_compiles("
    #include <cstddef>
    #include <cstdint>
    extern \"C\" int LLVMFuzzerTestOneInput(const std::uint8_t*, std::size_t) { return 0; }"
    HVYXG_HAVE_LIBFUZZER)
  unset(CMAKE_REQUIRED_FLAGS)
  unset(CMAKE_REQUIRED_LINK_OPTIONS)
  if(HVYXG_HAVE_LIBFUZZER)
    set(HVYXG_LIBFUZZER ON)
    add_compile_options(-fsanitize=fuzzer-no-link)
  endif()
endif()

include(GNUInstallDirs)

# hvyxg/version.hpp from the project version.
configure_file(include/hvyxg/version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/hvyxg/version.hpp @ONLY)

file(GLOB_RECURSE HVYXG_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(hvyxg STATIC ${HVYXG_SOURCES})
add_library(hvyxg::hvyxg ALIAS hvyxg)
# Public headers: include/hvyxg/ (#include "hvyxg/hvyxg.hpp", or the single headers) and the
# generated version.hpp; implementation: src/; vendored code: third_party/.
target_include_directories(hvyxg PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
                                        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_features(hvyxg PUBLIC cxx_std_20)
target_compile_options(hvyxg PRIVATE ${HVYXG_WARNINGS})
# The MU ROM converter must reproduce SXG-Create-NF's (Python) floating point exactly: no FMA
# contraction there (THIRD_PARTY.md, "SXG-Create-NF").
file(GLOB HVYXG_MU_ROM_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/syxg50/mu_rom/*.cpp)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  set_source_files_properties(${HVYXG_MU_ROM_SOURCES} PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
elseif(MSVC)
  set_source_files_properties(${HVYXG_MU_ROM_SOURCES} PROPERTIES COMPILE_OPTIONS "/fp:precise")
endif()

# The core: what an embedded host (Hira-V's xg.lib) builds, without exceptions, RTTI or files
# (HVYXG_NO_FILES). Built here as a check that it stays that way; nothing links it.
option(HVYXG_BUILD_CORE_CHECK "Build the core sources with -fno-exceptions -fno-rtti as a check" ON)
if(HVYXG_BUILD_CORE_CHECK AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  set(HVYXG_CORE_SOURCES ${HVYXG_SOURCES})
  list(FILTER HVYXG_CORE_SOURCES EXCLUDE REGEX "_files\\.cpp$|/bank_search\\.cpp$|/bank_io\\.cpp$|/plugin_tables\\.cpp$")
  add_library(hvyxg_core_check STATIC ${HVYXG_CORE_SOURCES})
  target_include_directories(hvyxg_core_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_BINARY_DIR}/generated
                             ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
  target_compile_definitions(hvyxg_core_check PRIVATE HVYXG_NO_FILES=1)
  target_compile_options(hvyxg_core_check PRIVATE ${HVYXG_WARNINGS} -fno-exceptions -fno-rtti)
  set_source_files_properties(${HVYXG_MU_ROM_SOURCES} TARGET_DIRECTORY hvyxg_core_check PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
  # Linked alone, so a core source that needs a hosted one fails here.
  add_executable(hvyxg_core_link tests/core/core_link.cpp)
  target_link_libraries(hvyxg_core_link PRIVATE hvyxg_core_check)
  target_include_directories(hvyxg_core_link PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_BINARY_DIR}/generated)
  target_compile_definitions(hvyxg_core_link PRIVATE HVYXG_NO_FILES=1)
  target_compile_options(hvyxg_core_link PRIVATE ${HVYXG_WARNINGS} -fno-exceptions -fno-rtti)
  if(HVYXG_BUILD_TESTS)
    enable_testing()
    add_test(NAME hvyxg_core_link COMMAND hvyxg_core_link)
  endif()
endif()

# Every public header compiles on its own, with and without HVYXG_NO_FILES.
file(GLOB_RECURSE HVYXG_PUBLIC_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/include CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/include/hvyxg/*.hpp)
set(HVYXG_HEADER_CHECK_SOURCES)
foreach(h ${HVYXG_PUBLIC_HEADERS} hvyxg/version.hpp)
  string(MAKE_C_IDENTIFIER ${h} id)
  set(f ${CMAKE_CURRENT_BINARY_DIR}/header_check/${id}.cpp)
  file(CONFIGURE OUTPUT ${f} CONTENT "#include \"${h}\"\n#include \"${h}\"\n")
  list(APPEND HVYXG_HEADER_CHECK_SOURCES ${f})
endforeach()
foreach(mode files nofiles)
  add_library(hvyxg_header_check_${mode} OBJECT ${HVYXG_HEADER_CHECK_SOURCES})
  target_include_directories(hvyxg_header_check_${mode} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                             ${CMAKE_CURRENT_BINARY_DIR}/generated)
  target_compile_options(hvyxg_header_check_${mode} PRIVATE ${HVYXG_WARNINGS})
endforeach()
target_compile_definitions(hvyxg_header_check_nofiles PRIVATE HVYXG_NO_FILES=1)

# Install: the library, the public headers and a CMake package (find_package(hvyxg) gives
# hvyxg::hvyxg).
install(TARGETS hvyxg EXPORT hvyxgTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/hvyxg DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} FILES_MATCHING PATTERN "*.hpp")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/generated/hvyxg/version.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/hvyxg)
install(EXPORT hvyxgTargets NAMESPACE hvyxg:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hvyxg)
include(CMakePackageConfigHelpers)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/hvyxgConfig.cmake
     CONTENT "include(\"\${CMAKE_CURRENT_LIST_DIR}/hvyxgTargets.cmake\")\n" @ONLY)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/hvyxgConfigVersion.cmake COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/hvyxgConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/hvyxgConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hvyxg)

# The render kit the command-line renderers of hvyxg and hvopl3 are built on (tools/render/: the
# host, and Hira-V's UMP interface and Standard MIDI File reader, copied). Not part of the library.
add_library(hvyxg_render_kit STATIC tools/render/render_host.cpp tools/render/lib/midifile.cpp)
target_include_directories(hvyxg_render_kit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tools/render ${CMAKE_CURRENT_SOURCE_DIR}/tools)
target_compile_options(hvyxg_render_kit PRIVATE ${HVYXG_WARNINGS})

if(HVYXG_BUILD_TOOLS)
  add_executable(render_yxg tools/render_yxg.cpp)
  target_link_libraries(render_yxg PRIVATE hvyxg hvyxg_render_kit)
  target_compile_options(render_yxg PRIVATE ${HVYXG_WARNINGS})

  add_executable(hvyxg-bench tools/hvyxg_bench.cpp)
  target_link_libraries(hvyxg-bench PRIVATE hvyxg hvyxg_render_kit)
  target_compile_options(hvyxg-bench PRIVATE ${HVYXG_WARNINGS})
  install(TARGETS render_yxg RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(HVYXG_BUILD_FUZZERS)
  set(HVYXG_FUZZ_TARGETS fuzz_syxg50_bank fuzz_mu_bank fuzz_syxg2006le_bank fuzz_mu_rom fuzz_midi fuzz_midi_stream fuzz_smf)
  add_library(hvyxg_fuzz_support STATIC fuzz/fuzz_support.cpp)
  target_link_libraries(hvyxg_fuzz_support PUBLIC hvyxg hvyxg_render_kit)
  target_include_directories(hvyxg_fuzz_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/fuzz ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_options(hvyxg_fuzz_support PRIVATE ${HVYXG_WARNINGS})
  if(NOT HVYXG_LIBFUZZER)
    add_library(hvyxg_fuzz_main STATIC fuzz/standalone_main.cpp)
    target_compile_options(hvyxg_fuzz_main PRIVATE ${HVYXG_WARNINGS})
  endif()
  foreach(t ${HVYXG_FUZZ_TARGETS})
    add_executable(${t} fuzz/${t}.cpp)
    target_link_libraries(${t} PRIVATE hvyxg_fuzz_support)
    target_compile_options(${t} PRIVATE ${HVYXG_WARNINGS})
    if(HVYXG_LIBFUZZER)
      target_link_options(${t} PRIVATE -fsanitize=fuzzer)
    else()
      target_link_libraries(${t} PRIVATE hvyxg_fuzz_main)
    endif()
  endforeach()
  # Replay the inputs of every bug the fuzzers found (fuzz/regressions/<target>/).
  enable_testing()
  foreach(t ${HVYXG_FUZZ_TARGETS})
    if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/regressions/${t})
      add_test(NAME ${t}_regressions COMMAND ${t} -runs=0 ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/regressions/${t})
    endif()
  endforeach()
  add_executable(hvyxg_fuzz_seeds fuzz/make_seeds.cpp)
  target_link_libraries(hvyxg_fuzz_seeds PRIVATE hvyxg_fuzz_support)
  target_compile_options(hvyxg_fuzz_seeds PRIVATE ${HVYXG_WARNINGS})
endif()

if(HVYXG_BUILD_TESTS)
  # doctest 2.4.11, vendored (third_party/doctest, MIT).
  add_library(hvyxg_doctest INTERFACE)
  target_include_directories(hvyxg_doctest SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

  # doctest's test-case macros use __COUNTER__, which recent Clang flags under -Wpedantic.
  set(HVYXG_TEST_WARNINGS ${HVYXG_WARNINGS})
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-Wc2y-extensions HVYXG_HAVE_WC2Y)
    if(HVYXG_HAVE_WC2Y)
      list(APPEND HVYXG_TEST_WARNINGS -Wno-c2y-extensions)
    endif()
  endif()

  file(GLOB HVYXG_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
  # fuzz_support.cpp: the synthetic banks (robustness tests, no bank files needed).
  add_executable(hvyxg_tests ${HVYXG_TEST_SOURCES} fuzz/fuzz_support.cpp)
  find_package(Threads REQUIRED)
  target_link_libraries(hvyxg_tests PRIVATE hvyxg hvyxg_doctest hvyxg_render_kit Threads::Threads)
  target_include_directories(hvyxg_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/fuzz
                             ${CMAKE_CURRENT_SOURCE_DIR}/third_party)  # tests reach internals
  target_compile_options(hvyxg_tests PRIVATE ${HVYXG_TEST_WARNINGS})
  # Default asset tree for the bank-dependent tests (git-ignored; see docs/TESTING.md).
  target_compile_definitions(hvyxg_tests PRIVATE HVYXG_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  enable_testing()
  add_test(NAME hvyxg_tests COMMAND hvyxg_tests)

  # Allocation check: its own binary, as it replaces the global operator new / delete. It
  # plays the synthetic banks of fuzz/ (no bank files needed) and the real ones when found.
  add_executable(hvyxg_alloc_test tests/alloc/alloc_test.cpp fuzz/fuzz_support.cpp)
  target_link_libraries(hvyxg_alloc_test PRIVATE hvyxg hvyxg_doctest)
  target_include_directories(hvyxg_alloc_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests
                                                     ${CMAKE_CURRENT_SOURCE_DIR}/fuzz)
  target_compile_options(hvyxg_alloc_test PRIVATE ${HVYXG_TEST_WARNINGS})
  target_compile_definitions(hvyxg_alloc_test PRIVATE HVYXG_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME hvyxg_alloc_test COMMAND hvyxg_alloc_test)
endif()
