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

# AddressSanitizer / UndefinedBehaviorSanitizer (GCC/Clang). Both default ON for
# a Debug build and OFF otherwise - the analog of the Makefile's OPT=0 vs OPT=1.
# Override either explicitly, e.g. -DASAN=OFF or -DUBSAN=ON. (No effect on MSVC,
# whose sanitizer flag spelling differs and which has no UBSan.)
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
   set(_san_default ON)
else()
   set(_san_default OFF)
endif()
set(ASAN  ${_san_default} CACHE BOOL "AddressSanitizer (GCC/Clang)")
set(UBSAN ${_san_default} CACHE BOOL "UndefinedBehaviorSanitizer (GCC/Clang)")

file(GLOB SOURCES ${GLOB_CONF_DEP} "src/*.cpp")
add_executable(mylang ${SOURCES})

target_include_directories(mylang PUBLIC "src")

if (TESTS)
   target_compile_definitions(mylang PUBLIC "TESTS")
endif()

# ASSERTS: the defense-in-depth assertion net (C assert + ML_CHECK, defs.h).
# Default ON for EVERY build type, so every CI run (GCC/Clang/MSVC, Debug AND
# Release) exercises it. ASSERTS=OFF defines NDEBUG to compile them all away.
# CMake's optimized configs add -DNDEBUG themselves; when ASSERTS is ON we
# append -UNDEBUG (target options come AFTER the config flags, and the last of
# -D/-U on the command line wins), cancelling it so Release keeps the asserts.
set(ASSERTS ON CACHE BOOL "Defense-in-depth assertions (assert + ML_CHECK)")
if (ASSERTS)
   if (MSVC)
      target_compile_options(mylang PUBLIC /UNDEBUG)
   else()
      target_compile_options(mylang PUBLIC -UNDEBUG)
      # libstdc++ container hardening: bounds-checks vector::operator[], string
      # ops, etc. that ASan only partially catches. ABI-safe (unlike
      # _GLIBCXX_DEBUG) and a no-op on libc++. Tied to ASSERTS so ASSERTS=0
      # removes it too (clean perf baseline).
      target_compile_definitions(mylang PUBLIC "_GLIBCXX_ASSERTIONS")
   endif()
else()
   target_compile_definitions(mylang PUBLIC "NDEBUG")
endif()

# Adversarial Construct allocator (forces address reuse so pointer-identity
# bugs are deterministic under -rt). Off by default; -DRECYCLE=ON. Parity with
# the Makefile's RECYCLE=1.
set(RECYCLE OFF CACHE BOOL "Adversarial recycling Construct allocator")
if (RECYCLE)
   target_compile_definitions(mylang PUBLIC "RECYCLE_ALLOC")
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

if (NOT MSVC)
   # The codebase relies on signed-overflow wraparound; mirror the Makefile's
   # -fwrapv so signed overflow is defined here too (see defs.h / README). MSVC
   # wraps by default and has no -fwrapv.
   target_compile_options(mylang PUBLIC -fwrapv)

   # Apply the sanitizers selected above, on the compile AND link lines. UBSan
   # excludes signed-integer-overflow: with -fwrapv that overflow is defined, so
   # the intentional wraparound is not a bug to flag.
   set(SAN_OPTS "")
   if (ASAN)
      list(APPEND SAN_OPTS -fsanitize=address)
   endif()
   if (UBSAN)
      # -fno-sanitize-recover makes a UBSan finding ABORT (non-zero exit) rather
      # than diagnose-and-continue - otherwise `mylang -rt` could hit UB, print
      # it, and still exit 0, so CI would pass on a real bug.
      list(APPEND SAN_OPTS -fsanitize=undefined
                           -fno-sanitize=signed-integer-overflow
                           -fno-sanitize-recover=undefined)
   endif()
   if (SAN_OPTS)
      list(APPEND SAN_OPTS -fno-omit-frame-pointer)
      target_compile_options(mylang PUBLIC ${SAN_OPTS})
      target_link_options(mylang PUBLIC ${SAN_OPTS})
   endif()
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
