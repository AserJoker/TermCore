# termcore_target_warnings(<target>)
#
# Applies the project warning set to a single target. Third-party targets
# (GoogleTest) keep their own flags and never go through this function.
function(termcore_target_warnings target)
  # The library deliberately uses the portable C API (fopen, getenv, ...) instead
  # of the MSVC-only *_s variants, so silence that family of CRT diagnostics.
  if(WIN32)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()

  # clang-cl / cl speak MSVC options; clang/gcc speak GNU options.
  if(MSVC OR CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(${target} PRIVATE /W4 /utf-8)
    if(TERMCORE_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(TERMCORE_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
