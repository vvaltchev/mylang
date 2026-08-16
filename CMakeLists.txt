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
set(WERROR        ON  CACHE BOOL   "Warnings are errors (-Werror / MSVC /WX)")

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

# The interpreter recurses on the C stack - the tree-walker (do_eval) and the
# bytecode VM (vm_run_chunk re-entered per recursive call). Deep recursion (e.g.
# the ackermann test) needs a stack bigger than WINDOWS' 1 MB default (Linux and
# macOS default to 8 MB, which is why they never hit this). Give the MSVC binary
# an 8 MB stack RESERVE for parity - a legitimate provisioning for an
# interpreter, not a workaround. (MSVC Debug's larger per-frame overhead - no
# scoped-local overlap + /RTC guards + a big try/catch EH frame in vm_run_chunk -
# makes the VM's deep recursion overflow 1 MB where GCC/clang fit; see the
# vm_run_chunk stack note in CLAUDE.md.)
if (MSVC)
   target_link_options(mylang PUBLIC "/STACK:8388608")
endif()

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

# VM dispatch: computed-goto (direct-threaded) on GCC/clang, default ON.
# -DCGOTO=OFF forces the portable switch dispatch (what MSVC always uses) -
# the A/B lever for the threaded dispatch. Parity with the Makefile's CGOTO.
set(CGOTO ON CACHE BOOL "Computed-goto VM dispatch (GCC/clang)")
if (NOT CGOTO)
   target_compile_definitions(mylang PUBLIC "ML_NO_CGOTO")
endif()

# VM hardening (ML_VM_CHECK): HEAVY per-op VM invariants - a frame-slot bounds
# check on every register access + an operand type-tag check. Default follows
# the build type (ON for Debug, OFF for an optimized build, for speed);
# -DVM_HARDENING=ON forces it on. CI enables it in the RELEASE lanes so a CI
# release exposes a layout-dependent UB as a loud, deterministic check rather
# than a mystery segfault. Parity with the Makefile's VM_HARDENING; gated by
# ASSERTS (a no-op under NDEBUG).
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
   set(VM_HARDENING_DEFAULT ON)
else()
   set(VM_HARDENING_DEFAULT OFF)
endif()
set(VM_HARDENING ${VM_HARDENING_DEFAULT} CACHE BOOL "Heavy per-op VM checks")
if (VM_HARDENING)
   target_compile_definitions(mylang PUBLIC "ML_VM_HARDENING=1")
else()
   target_compile_definitions(mylang PUBLIC "ML_VM_HARDENING=0")
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
   if (GCOV)
      target_compile_options(mylang PUBLIC -fprofile-arcs -ftest-coverage)
      target_link_options(mylang PUBLIC  -fprofile-arcs -ftest-coverage)
   endif()
endif()

# --- Warnings ---------------------------------------------------------------
# CMake sets no -Wall by default; mirror the Makefile's warning set for
# GCC/Clang so CI catches the same warnings a local build does. MSVC's CMake
# default level is already /W3.  (Order matters below: -Wall first, then the
# -Wno override, then -Werror last.)
if (NOT MSVC)
   target_compile_options(mylang PUBLIC -Wall -Wextra -Wno-unused-parameter)
endif()

# WERROR (default ON, EVERY build type + compiler): a warning FAILS the build,
# so it must be ADDRESSED, not left to accumulate in the log (see CLAUDE.md's
# warning rule - MSVC in particular emits narrowing/deprecation warnings the
# others don't). MSVC: /WX; GCC/Clang: -Werror. -DWERROR=OFF opts out.
if (WERROR)
   if (MSVC)
      target_compile_options(mylang PUBLIC /WX)
   else()
      target_compile_options(mylang PUBLIC -Werror)
   endif()
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
      set(ml_lto_on TRUE)
   else()
      message(STATUS "LTO requested but not supported: ${ipo_msg}")
   endif()
endif()

# Tell the binary what actually happened, so `-v` can report it. The Makefile
# has always defined ML_LTO; CMake did not, so a CMake build answered
# `lto unknown` - and a CI lane cannot ASSERT a config it cannot read, which
# is exactly what the non-LTO lane needs to do to avoid going vacuous.
#
# A generator expression, not a plain define, because IPO here is PER-CONFIG:
# only Release and RelWithDebInfo get it, so a multi-config generator must
# answer differently per config. `ml_lto_on` is false whenever LTO was off,
# GCOV was on, CMake was too old, or check_ipo_supported said no - so the
# macro tracks the OUTCOME, never the request.
if (ml_lto_on)
   target_compile_definitions(mylang PRIVATE
      $<IF:$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>,ML_LTO=1,ML_LTO=0>)
else()
   target_compile_definitions(mylang PRIVATE ML_LTO=0)
endif()
