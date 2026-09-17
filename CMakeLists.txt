cmake_minimum_required(VERSION 3.21)
project(termcore
        VERSION 0.1.0
        DESCRIPTION "Cross-platform terminal runtime library (C11)"
        LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Language standards
#
# The public library is C11; only the test suite is C++20 (GoogleTest).
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(TERMCORE_BUILD_TESTS    "Build the GoogleTest suite"        ON)
option(TERMCORE_BUILD_EXAMPLES "Build the examples"                ON)
option(TERMCORE_BUILD_SHARED   "Build termcore as a shared library" OFF)
option(TERMCORE_WERROR         "Treat warnings as errors"          OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(TermCoreWarnings)
include(TermCoreDependencies)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ---------------------------------------------------------------------------
# Library
#
# Sources are globbed per layer so that dropping a new *.c file into src/<layer>/
# needs no CMake edit (CONFIGURE_DEPENDS re-globs on configure).
#
#   src/common/    status, version
#   src/memory/    allocator
#   src/platform/  OS backends (win32 / posix)
#   src/control/   control layer   (see docs/02)
#   src/caps/      capability layer (see docs/03)
#   src/input/     input layer      (see docs/04)
#   src/text/      text engine      (see docs/05)
#   src/render/    render layer     (see docs/06)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE TERMCORE_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")

if(NOT TERMCORE_SOURCES)
  message(FATAL_ERROR "No sources found under src/ — nothing to build.")
endif()

if(TERMCORE_BUILD_SHARED)
  add_library(termcore SHARED ${TERMCORE_SOURCES})
  target_compile_definitions(termcore PUBLIC TERMCORE_USE_SHARED)
  target_compile_definitions(termcore PRIVATE TERMCORE_BUILD_SHARED)
  set_target_properties(termcore PROPERTIES
      C_VISIBILITY_PRESET hidden
      CXX_VISIBILITY_PRESET hidden
      WINDOWS_EXPORT_ALL_SYMBOLS OFF)
else()
  add_library(termcore STATIC ${TERMCORE_SOURCES})
endif()

add_library(termcore::termcore ALIAS termcore)

target_include_directories(termcore PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>")

termcore_target_warnings(termcore)

# Platform libraries. POSIX needs nothing extra yet; Windows console APIs are
# linked lazily by the platform layer as they land.
if(WIN32)
  target_compile_definitions(termcore PRIVATE WIN32_LEAN_AND_MEAN)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(TERMCORE_BUILD_EXAMPLES)
  add_executable(termcore_hello examples/hello.c)
  target_link_libraries(termcore_hello PRIVATE termcore::termcore)
  termcore_target_warnings(termcore_hello)
endif()

# ---------------------------------------------------------------------------
# Tests (GoogleTest)
# ---------------------------------------------------------------------------
if(TERMCORE_BUILD_TESTS)
  enable_testing()
  termcore_resolve_gtest(TERMCORE_GTEST_TARGET)

  file(GLOB TERMCORE_TEST_SOURCES CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")

  add_executable(termcore_test ${TERMCORE_TEST_SOURCES})
  target_link_libraries(termcore_test PRIVATE termcore::termcore ${TERMCORE_GTEST_TARGET})
  termcore_target_warnings(termcore_test)

  include(GoogleTest)
  gtest_discover_tests(termcore_test)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS termcore
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/termcore DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
