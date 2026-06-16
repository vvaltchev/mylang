# SPDX-License-Identifier: BSD-2-Clause
cmake_minimum_required(VERSION 3.8)

project(mylang CXX)

set(CMAKE_VERBOSE_MAKEFILE off)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Honor INTERPROCEDURAL_OPTIMIZATION (LTO) target properties. Set before the
# target is created. Guarded so the 3.8 minimum still configures (the LTO block
# below is itself gated on CMake >= 3.9, where this policy exists).
if (POLICY CMP0069)
   cmake_policy(SET CMP0069 NEW)
endif()

if (NOT ${CMAKE_VERSION} VERSION_LESS "3.12.0")
   set(GLOB_CONF_DEP CONFIGURE_DEPENDS)
endif()

set(TESTS         OFF CACHE BOOL   "Compile-in unit tests")
set(GCOV          OFF CACHE BOOL   "Prepare a coverage build")
set(LTO           ON  CACHE BOOL   "Link-time optimization (optimized configs)")

file(GLOB SOURCES ${GLOB_CONF_DEP} "src/*.cpp")
add_executable(mylang ${SOURCES})

target_include_directories(mylang PUBLIC "src")

if (TESTS)
   target_compile_definitions(mylang PUBLIC "TESTS")
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
   if (GCOV)
      target_compile_options(mylang PUBLIC -fprofile-arcs -ftest-coverage)
      target_link_options(mylang PUBLIC  -fprofile-arcs -ftest-coverage)
   endif()
endif()

# The codebase intentionally calls `move`/`forward` unqualified (see
# `using std::move;` in defs.h), which recent clang reports under
# -Wunqualified-std-cast-call. Silence it for clang only.
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
   target_compile_options(mylang PUBLIC -Wno-unqualified-std-cast-call)
endif()

# Whole-program (link-time) optimization for optimized configs, on by default:
# ~7% smaller binary and ~8-9% faster on the bench suite. Configure with
# -DLTO=OFF to disable. INTERPROCEDURAL_OPTIMIZATION abstracts over GCC/Clang
# (-flto) and MSVC (/GL /LTCG). Only Release/RelWithDebInfo get it; Debug and
# coverage builds are left alone. (check_ipo_supported needs CMake >= 3.9.)
if (LTO AND NOT GCOV AND NOT CMAKE_VERSION VERSION_LESS "3.9")
   include(CheckIPOSupported)
   check_ipo_supported(RESULT ipo_ok OUTPUT ipo_msg LANGUAGES CXX)
   if (ipo_ok)
      set_property(TARGET mylang
                   PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
      set_property(TARGET mylang
                   PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
   else()
      message(STATUS "LTO requested but not supported: ${ipo_msg}")
   endif()
endif()
