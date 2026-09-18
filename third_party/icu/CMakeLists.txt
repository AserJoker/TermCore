# =============================================================================
# ICU build via FetchContent (no git submodule), EXTERNAL DATA MODE
#
# Source + common data are obtained from (in priority order):
#   1. ICU_LOCAL_SRC_ZIP  / ICU_LOCAL_DATA_ZIP  / ICU_LOCAL_DATA_DAT  (pre-downloaded)
#   2. otherwise downloaded from the ICU GitHub release for ICU_TAG
# So you can build fully offline by pointing the ICU_LOCAL_*_ZIP vars at local
# archives, e.g.:
#   -DICU_LOCAL_SRC_ZIP=D:/projects/termcore/icu4c-74_2-src.tgz
#   -DICU_LOCAL_DATA_ZIP=D:/projects/termcore/icu4c-74_2-data-bin-l.zip
#   (or -DICU_LOCAL_DATA_DAT=D:/projects/termcore/icudt74l.dat)
# When a local source zip of a DIFFERENT ICU version is used, also provide the
# matching data via ICU_LOCAL_DATA_ZIP / ICU_LOCAL_DATA_DAT, because the network
# data URL is derived from ICU_TAG and would otherwise not match the headers.
#
# The libraries are compiled manually as static archives. Unlike clux's ICU
# integration, the icudt*.dat package is NOT converted into a C byte array and
# embedded into the binary; it is copied verbatim to
#   ${CMAKE_BINARY_DIR}/data/icudt<ver><e>.dat
# and loaded at runtime. The consumer must call icu_data_init(data_dir) once at
# startup (see icu_data.h / icu_data.c), passing that directory.
#
# Exposes alias targets following the vcpkg convention:
#   target_link_libraries(app PRIVATE ICU::i18n ICU::uc ICU::data ICU::io)
# =============================================================================

include(FetchContent)

# ---------------------------------------------------------------------------
# User-tunable options
# ---------------------------------------------------------------------------
set(ICU_TAG "release-74-2" CACHE STRING "ICU GitHub release tag used for the network download.")
set(ICU_LOCAL_SRC_ZIP   "" CACHE FILEPATH "Local ICU source zip; if set & exists, used instead of downloading.")
set(ICU_LOCAL_DATA_ZIP  "" CACHE FILEPATH "Local ICU data zip; if set & exists, used instead of downloading.")
set(ICU_LOCAL_DATA_DAT  "" CACHE FILEPATH "Local prebuilt icudt*.dat; if set & exists, used directly (no extraction).")

# Derive version-specific archive names from ICU_TAG (release-74-2 -> 74_2)
string(REGEX REPLACE "^release-" "" ICU_VER "${ICU_TAG}")
string(REPLACE "-" "_" ICU_VER_US "${ICU_VER}")
set(ICU_SRC_URL  "https://github.com/unicode-org/icu/releases/download/${ICU_TAG}/icu4c-${ICU_VER_US}-src.tgz")
set(ICU_DATA_URL "https://github.com/unicode-org/icu/releases/download/${ICU_TAG}/icu4c-${ICU_VER_US}-data-bin-l.zip")

# ---------------------------------------------------------------------------
# Convention directory: third_party/icu/download/
# Drop the archives there (icu4c-*-src.tgz, icu4c-*-data-bin-l.zip)
# and they will be picked up automatically, no CMake variables needed.
# ---------------------------------------------------------------------------
set(ICU_DOWNLOAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/download")
if(IS_DIRECTORY "${ICU_DOWNLOAD_DIR}")
  if(NOT ICU_LOCAL_SRC_ZIP OR NOT EXISTS "${ICU_LOCAL_SRC_ZIP}")
    file(GLOB _dl_src "${ICU_DOWNLOAD_DIR}/icu4c-*-src.tgz")
    if(_dl_src)
      list(GET _dl_src 0 ICU_LOCAL_SRC_ZIP)
      message(STATUS "ICU: auto-detected local source from download dir: ${ICU_LOCAL_SRC_ZIP}")
    endif()
  endif()
  if(NOT ICU_LOCAL_DATA_ZIP OR NOT EXISTS "${ICU_LOCAL_DATA_ZIP}")
    file(GLOB _dl_data "${ICU_DOWNLOAD_DIR}/icu4c-*-data-bin-l.zip")
    if(_dl_data)
      list(GET _dl_data 0 ICU_LOCAL_DATA_ZIP)
      message(STATUS "ICU: auto-detected local data from download dir: ${ICU_LOCAL_DATA_ZIP}")
    endif()
  endif()
  if(NOT ICU_LOCAL_DATA_DAT OR NOT EXISTS "${ICU_LOCAL_DATA_DAT}")
    file(GLOB _dl_dat "${ICU_DOWNLOAD_DIR}/icudt*.dat")
    if(_dl_dat)
      list(GET _dl_dat 0 ICU_LOCAL_DATA_DAT)
      message(STATUS "ICU: auto-detected local dat from download dir: ${ICU_LOCAL_DATA_DAT}")
    endif()
  endif()
endif()

# ---------------------------------------------------------------------------
# Fetch ICU source (icu4c); prefer a local zip when provided
# ---------------------------------------------------------------------------
set(_src_url "${ICU_SRC_URL}")
if(ICU_LOCAL_SRC_ZIP AND EXISTS "${ICU_LOCAL_SRC_ZIP}")
  set(_src_url "${ICU_LOCAL_SRC_ZIP}")
  message(STATUS "ICU: using local source zip ${_src_url}")
else()
  message(STATUS "ICU: downloading source from ${ICU_SRC_URL}")
endif()
FetchContent_Declare(
  icu_src
  URL ${_src_url}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(icu_src)

# Locate the 'source' directory inside the extracted archive (layout may vary).
if(EXISTS "${icu_src_SOURCE_DIR}/source")
  set(ICU_ROOT "${icu_src_SOURCE_DIR}/source")
else()
  file(GLOB _cand "${icu_src_SOURCE_DIR}/*/source")
  if(_cand)
    list(GET _cand 0 ICU_ROOT)
  else()
    set(ICU_ROOT "${icu_src_SOURCE_DIR}")
  endif()
endif()
message(STATUS "ICU root: ${ICU_ROOT}")

# ---------------------------------------------------------------------------
# Fetch / locate ICU common data (.dat) — kept as an EXTERNAL file, not embedded
# ---------------------------------------------------------------------------
set(ICU_DATA_DAT "")
if(ICU_LOCAL_DATA_DAT AND EXISTS "${ICU_LOCAL_DATA_DAT}")
  set(ICU_DATA_DAT "${ICU_LOCAL_DATA_DAT}")
  message(STATUS "ICU: using local data file ${ICU_DATA_DAT}")
else()
  set(_data_url "${ICU_DATA_URL}")
  if(ICU_LOCAL_DATA_ZIP AND EXISTS "${ICU_LOCAL_DATA_ZIP}")
    set(_data_url "${ICU_LOCAL_DATA_ZIP}")
    message(STATUS "ICU: using local data zip ${_data_url}")
  else()
    message(STATUS "ICU: downloading data from ${ICU_DATA_URL}")
  endif()
  FetchContent_Declare(
    icu_data
    URL ${_data_url}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(icu_data)
  file(GLOB_RECURSE ICU_DATA_DAT "${icu_data_SOURCE_DIR}/icudt*l.dat")
endif()

if(NOT ICU_DATA_DAT)
  message(FATAL_ERROR "icudt*.dat not found (checked local override and ${ICU_DATA_URL})")
endif()
list(GET ICU_DATA_DAT 0 ICU_DATA_DAT)
message(STATUS "ICU data file: ${ICU_DATA_DAT}")

# ---------------------------------------------------------------------------
# Stage the data package for runtime loading. The .dat is copied (not compiled)
# into the build tree; icu_data_init() is pointed at this directory at runtime.
# ---------------------------------------------------------------------------
set(ICU_DATA_DIR "${CMAKE_BINARY_DIR}/data" CACHE PATH
    "Directory where the ICU data package (icudt*.dat) is staged for runtime load.")
file(MAKE_DIRECTORY "${ICU_DATA_DIR}")
get_filename_component(ICU_DATA_FILE_NAME "${ICU_DATA_DAT}" NAME)
set(ICU_DATA_FILE "${ICU_DATA_DIR}/${ICU_DATA_FILE_NAME}")
if(NOT EXISTS "${ICU_DATA_FILE}" OR NOT "${ICU_DATA_DAT}" STREQUAL "${ICU_DATA_FILE}")
  file(COPY "${ICU_DATA_DAT}" DESTINATION "${ICU_DATA_DIR}")
endif()
message(STATUS "ICU data staged for runtime load: ${ICU_DATA_FILE}")

# ---------------------------------------------------------------------------
# Collect source files
# ---------------------------------------------------------------------------
file(GLOB ICU_COMMON_SRC
  ${ICU_ROOT}/common/*.cpp
  ${ICU_ROOT}/common/*.c
)
file(GLOB ICU_I18N_SRC
  ${ICU_ROOT}/i18n/*.cpp
  ${ICU_ROOT}/i18n/*.c
)
file(GLOB ICU_IO_SRC
  ${ICU_ROOT}/io/*.cpp
  ${ICU_ROOT}/io/*.c
)

# ---------------------------------------------------------------------------
# PUBLIC defines - must be visible to both ICU and consumers
# ---------------------------------------------------------------------------
set(ICU_PUBLIC_DEFS
  U_STATIC_IMPLEMENTATION
  U_DISABLE_RENAMING=1
  U_DISABLE_VERSION_SUFFIX=1
  U_USING_ICU_NAMESPACE=0
)
if(WIN32)
  list(APPEND ICU_PUBLIC_DEFS
    U_PLATFORM_USES_ONLY_WIN32_API=1
    U_PLATFORM_HAS_WINUWP_API=0
  )
endif()

# ---------------------------------------------------------------------------
# PRIVATE (per-library implementation) defines
# ---------------------------------------------------------------------------
set(ICU_COMMON_DEF U_COMMON_IMPLEMENTATION)
set(ICU_I18N_DEF  U_I18N_IMPLEMENTATION)
set(ICU_IO_DEF    U_IO_IMPLEMENTATION)

# ---------------------------------------------------------------------------
# ICU Common Library (libicuuc)
# ---------------------------------------------------------------------------
add_library(icuuc STATIC ${ICU_COMMON_SRC})
target_include_directories(icuuc PUBLIC ${ICU_ROOT}/common)
target_compile_definitions(icuuc
  PUBLIC  ${ICU_PUBLIC_DEFS}
  PRIVATE ${ICU_COMMON_DEF}
)
if(MSVC)
  target_compile_definitions(icuuc PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# ICU Data Library (libicudata)
#   - stubdata.cpp : minimal stub entry point (no data embedded)
#   - icudataver.cpp : data version info
# The real icudt*.dat is loaded from ICU_DATA_DIR at runtime (icu_data_init).
# ---------------------------------------------------------------------------
add_library(icudata STATIC
  ${ICU_ROOT}/stubdata/stubdata.cpp
  ${ICU_ROOT}/common/icudataver.cpp
)
target_include_directories(icudata PUBLIC ${ICU_ROOT}/common)
target_compile_definitions(icudata
  PUBLIC  ${ICU_PUBLIC_DEFS}
)
if(MSVC)
  target_compile_definitions(icudata PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# ICU I18N Library (libicui18n)
# ---------------------------------------------------------------------------
add_library(icui18n STATIC ${ICU_I18N_SRC})
target_include_directories(icui18n PUBLIC ${ICU_ROOT}/i18n)
target_link_libraries(icui18n PUBLIC icuuc icudata)
target_compile_definitions(icui18n
  PUBLIC  ${ICU_PUBLIC_DEFS}
  PRIVATE ${ICU_I18N_DEF}
)
if(MSVC)
  target_compile_definitions(icui18n PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# ICU IO Library (libicuio)
# ---------------------------------------------------------------------------
add_library(icuio STATIC ${ICU_IO_SRC})
target_include_directories(icuio PUBLIC ${ICU_ROOT}/io)
target_link_libraries(icuio PUBLIC icuuc icui18n)
target_compile_definitions(icuio
  PUBLIC  ${ICU_PUBLIC_DEFS}
  PRIVATE ${ICU_IO_DEF}
)
if(MSVC)
  target_compile_definitions(icuio PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# Data bootstrap helper: consumer compiles icu_data.c and calls
# icu_data_init(data_dir) once at startup to register the data directory.
# ---------------------------------------------------------------------------
add_library(icu_data_init STATIC ${CMAKE_CURRENT_SOURCE_DIR}/icu_data.c)
target_include_directories(icu_data_init PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(icu_data_init PUBLIC icudata icuuc)

# ---------------------------------------------------------------------------
# Alias targets matching vcpkg convention
# ---------------------------------------------------------------------------
add_library(ICU::uc   ALIAS icuuc)
add_library(ICU::i18n ALIAS icui18n)
add_library(ICU::data ALIAS icudata)
add_library(ICU::io   ALIAS icuio)

# ---------------------------------------------------------------------------
# Suppress ICU warnings (third-party code)
# ---------------------------------------------------------------------------
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  foreach(_tgt icuuc icudata icui18n icuio)
    target_compile_options(${_tgt} PRIVATE -w)
  endforeach()
elseif(MSVC)
  foreach(_tgt icuuc icudata icui18n icuio)
    # /utf-8: ICU sources embed UTF-8 string literals (e.g. static_unicode_sets.h)
    target_compile_options(${_tgt} PRIVATE /W0 /utf-8)
  endforeach()
endif()

message(STATUS "ICU build targets configured (external data mode, FetchContent)")
message(STATUS "ICU data directory for runtime: ${ICU_DATA_DIR}")
