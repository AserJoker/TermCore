include_guard(GLOBAL)

include(FetchContent)

# GoogleTest acquisition strategy.
#
#   LOCAL  — use third_party/googletest (offline; drop the sources there)
#   FETCH  — FetchContent a pinned tag (needs network on first configure)
#   SYSTEM — find_package(GTest) (vcpkg / distro packages)
#   AUTO   — LOCAL when third_party/googletest exists, otherwise FETCH
set(TERMCORE_GTEST_SOURCE "AUTO" CACHE STRING
    "Where GoogleTest comes from: AUTO | LOCAL | FETCH | SYSTEM")
set_property(CACHE TERMCORE_GTEST_SOURCE PROPERTY STRINGS AUTO LOCAL FETCH SYSTEM)

set(TERMCORE_GTEST_TAG "v1.17.0" CACHE STRING "GoogleTest tag used by FETCH")
set(TERMCORE_GTEST_URL "https://github.com/google/googletest.git" CACHE STRING
    "GoogleTest repository URL used by FETCH")

# termcore_resolve_gtest(<out-var>) — sets <out-var> to the GTest target name.
function(termcore_resolve_gtest out_var)
  set(mode "${TERMCORE_GTEST_SOURCE}")

  if(mode STREQUAL "AUTO")
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../third_party/googletest/CMakeLists.txt")
      set(mode "LOCAL")
    else()
      set(mode "FETCH")
    endif()
  endif()

  # gtest must use the same (dynamic) CRT as the rest of the project, otherwise
  # linking fails with a RuntimeLibrary mismatch on Windows. No-op elsewhere.
  set(gtest_force_shared_crt ON CACHE BOOL "Use shared CRT for gtest" FORCE)

  if(mode STREQUAL "LOCAL")
    message(STATUS "GoogleTest: LOCAL (third_party/googletest)")
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../third_party/googletest"
                     "${CMAKE_BINARY_DIR}/_deps/googletest-build" EXCLUDE_FROM_ALL)
  elseif(mode STREQUAL "SYSTEM")
    message(STATUS "GoogleTest: SYSTEM (find_package)")
    find_package(GTest REQUIRED)
  elseif(mode STREQUAL "FETCH")
    message(STATUS "GoogleTest: FETCH (${TERMCORE_GTEST_URL} @ ${TERMCORE_GTEST_TAG})")
    FetchContent_Declare(googletest
        GIT_REPOSITORY "${TERMCORE_GTEST_URL}"
        GIT_TAG        "${TERMCORE_GTEST_TAG}"
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE)
    FetchContent_MakeAvailable(googletest)
  else()
    message(FATAL_ERROR "Unknown TERMCORE_GTEST_SOURCE='${TERMCORE_GTEST_SOURCE}'")
  endif()

  set(${out_var} GTest::gtest PARENT_SCOPE)
endfunction()
