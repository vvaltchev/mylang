# SPDX-License-Identifier: BSD-2-Clause
cmake_minimum_required(VERSION 3.8)

project(mylang CXX)

set(CMAKE_VERBOSE_MAKEFILE off)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT ${CMAKE_VERSION} VERSION_LESS "3.12.0")
   set(GLOB_CONF_DEP CONFIGURE_DEPENDS)
endif()

set(TESTS         OFF CACHE BOOL   "Compile-in unit tests")
set(GCOV          OFF CACHE BOOL   "Prepare a coverage build")

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
