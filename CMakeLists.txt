cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# Version, derived from the latest git tag (e.g. v1.0.0). Falls back to the
# constant below when building outside a git checkout or with no tags present.
# CFBundleShortVersionString needs a clean X.Y.Z; the full describe string
# (which may include a commit count / hash / -dirty) is used for the build id.
# ---------------------------------------------------------------------------
set(ARGUS_VERSION_FALLBACK "1.1.0")
set(ARGUS_VERSION "${ARGUS_VERSION_FALLBACK}")
set(ARGUS_VERSION_FULL "${ARGUS_VERSION_FALLBACK}")
find_package(Git QUIET)
if(Git_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --dirty --always
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE _git_desc OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _git_res ERROR_QUIET)
    if(_git_res EQUAL 0 AND _git_desc)
        set(ARGUS_VERSION_FULL "${_git_desc}")
        if(_git_desc MATCHES "([0-9]+\\.[0-9]+\\.[0-9]+)")
            set(ARGUS_VERSION "${CMAKE_MATCH_1}")
        endif()
    endif()
endif()
message(STATUS "argus version: ${ARGUS_VERSION} (build ${ARGUS_VERSION_FULL})")

project(argus VERSION ${ARGUS_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(ARGUS_BUILD_GUI "Build the Dear ImGui desktop application" OFF)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
# libsndfile (decoding). Prefer CMake config, fall back to pkg-config.
find_package(SndFile QUIET CONFIG)
if(NOT SndFile_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SNDFILE REQUIRED sndfile)
endif()

# libebur128 (ITU-R BS.1770 loudness / true peak). Fetched and built from source.
include(FetchContent)
FetchContent_Declare(
    ebur128
    GIT_REPOSITORY https://github.com/jiixyj/libebur128.git
    GIT_TAG v1.2.6
    GIT_SHALLOW TRUE
)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# libebur128 1.2.6 ships an old cmake_minimum_required; allow it under CMake 4.x.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
FetchContent_MakeAvailable(ebur128)

# ---------------------------------------------------------------------------
# Analysis engine (headless, no UI dependency)
# ---------------------------------------------------------------------------
add_library(argus_core STATIC
    src/core/Decoder.cpp
    src/core/Report.cpp
    src/core/Engine.cpp
    src/core/Profile.cpp
    src/core/ProfileStore.cpp
    src/core/ChannelLayout.cpp
    src/core/dsp/Envelope.cpp
    src/core/dsp/Fft.cpp
    src/core/analysis/Metadata.cpp
    src/core/analysis/Clipping.cpp
    src/core/analysis/Dropouts.cpp
    src/core/analysis/Clicks.cpp
    src/core/analysis/Silence.cpp
    src/core/analysis/Phase.cpp
    src/core/analysis/Loudness.cpp
    src/core/analysis/Spectral.cpp
    src/core/render/Spectrogram.cpp
    src/core/export/CsvExport.cpp
    src/core/export/PdfExport.cpp
    src/core/export/EmbeddedFonts.cpp
    src/core/export/EmbeddedLogo.cpp
    src/core/export/JsonExport.cpp
    src/core/export/PngExport.cpp
)
target_include_directories(argus_core PUBLIC src)
target_include_directories(argus_core PRIVATE third_party/stb)
target_compile_definitions(argus_core PUBLIC ARGUS_VERSION="${ARGUS_VERSION_FULL}")

# ---------------------------------------------------------------------------
# Compressed-audio fallback decoder (AAC / Apple Lossless that libsndfile can't
# open). Exactly one platform implementation is compiled.
# ---------------------------------------------------------------------------
if(APPLE)
    target_sources(argus_core PRIVATE src/core/decode/DecodeApple.cpp)
    target_link_libraries(argus_core PUBLIC "-framework AudioToolbox" "-framework CoreFoundation")
elseif(WIN32)
    target_sources(argus_core PRIVATE src/core/decode/DecodeWindows.cpp)
    target_link_libraries(argus_core PUBLIC mfplat mfreadwrite mfuuid ole32)
else()
    # Linux / other: use FFmpeg if available, otherwise a no-op stub.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFMPEG IMPORTED_TARGET libavformat libavcodec libavutil)
    endif()
    if(FFMPEG_FOUND)
        target_sources(argus_core PRIVATE src/core/decode/DecodeFfmpeg.cpp)
        target_compile_definitions(argus_core PRIVATE ARGUS_HAVE_FFMPEG=1)
        target_link_libraries(argus_core PUBLIC PkgConfig::FFMPEG)
        message(STATUS "argus: AAC/ALAC decode via FFmpeg")
    else()
        target_sources(argus_core PRIVATE src/core/decode/DecodeNone.cpp)
        message(STATUS "argus: FFmpeg not found - AAC/ALAC decode disabled")
    endif()
endif()

if(SndFile_FOUND)
    target_link_libraries(argus_core PUBLIC SndFile::sndfile)
else()
    target_include_directories(argus_core PUBLIC ${SNDFILE_INCLUDE_DIRS})
    target_link_libraries(argus_core PUBLIC ${SNDFILE_LINK_LIBRARIES})
    target_link_directories(argus_core PUBLIC ${SNDFILE_LIBRARY_DIRS})
endif()

target_link_libraries(argus_core PUBLIC ebur128)
# libebur128 uses directory-scoped includes, so expose its header to consumers.
target_include_directories(argus_core PUBLIC ${ebur128_SOURCE_DIR}/ebur128)

# ---------------------------------------------------------------------------
# CLI front-end
# ---------------------------------------------------------------------------
add_executable(argus
    src/main.cpp
    src/cli/ConsoleReport.cpp
)
target_link_libraries(argus PRIVATE argus_core)

include(GNUInstallDirs)
install(TARGETS argus RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(ARGUS_BUILD_GUI)
    add_subdirectory(src/ui)
endif()
