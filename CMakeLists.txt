cmake_minimum_required(VERSION 3.16)
project(WhisperWakeWordEngine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# Build options
option(BUILD_TESTING "Build the unit tests" ON)

# ---------------------------------------------------------------------------
# Dependencies: RtAudio via pkg-config; Whisper + GGML via pkg-config with
# find_library fallback (no hard-coded Homebrew Cellar versions).
# ---------------------------------------------------------------------------
pkg_check_modules(RTAUDIO REQUIRED IMPORTED_TARGET rtaudio)

# Whisper - pkg-config may reference a stale/moved Homebrew Cellar path
# (the formula upgrades the keg but the .pc prefix can point at the wrong one),
# so validate the include dir and fall back to find_library + brew.
pkg_check_modules(WHISPER QUIET IMPORTED_TARGET whisper)
if(WHISPER_FOUND AND EXISTS "${WHISPER_INCLUDE_DIRS}/whisper.h")
  set(WHISPER_LIB "")
else()
  set(WHISPER_FOUND OFF)
  find_library(WHISPER_LIB NAMES whisper HINTS /opt/homebrew/lib /usr/local/lib)
  if(NOT WHISPER_LIB)
    execute_process(COMMAND brew --prefix whisper-cpp
                    OUTPUT_VARIABLE WHISPER_KEG OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    find_library(WHISPER_LIB NAMES whisper HINTS ${WHISPER_KEG}/lib)
  endif()
endif()

# GGML - same robustness as whisper.
pkg_check_modules(GGML QUIET IMPORTED_TARGET ggml)
if(GGML_FOUND AND EXISTS "${GGML_INCLUDE_DIRS}/ggml-backend.h")
  set(GGML_LIB "")
else()
  set(GGML_FOUND OFF)
  find_library(GGML_LIB NAMES ggml ggml-base HINTS /opt/homebrew/lib /usr/local/lib)
  if(NOT GGML_LIB)
    execute_process(COMMAND brew --prefix ggml
                    OUTPUT_VARIABLE GGML_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    find_library(GGML_LIB NAMES ggml ggml-base HINTS ${GGML_PREFIX}/lib)
  endif()
endif()

if(NOT WHISPER_FOUND AND NOT WHISPER_LIB)
  message(FATAL_ERROR "whisper was not found. Install via: brew install whisper-cpp")
endif()
if(NOT GGML_FOUND AND NOT GGML_LIB)
  message(FATAL_ERROR "ggml was not found. Install via: brew install ggml")
endif()

find_library(ACCELERATE_FRAMEWORK Accelerate)
find_library(METAL_FRAMEWORK Metal)
find_library(FOUNDATION_FRAMEWORK Foundation)
if(NOT ACCELERATE_FRAMEWORK OR NOT METAL_FRAMEWORK OR NOT FOUNDATION_FRAMEWORK)
  message(FATAL_ERROR "Required Apple framework not found (Accelerate/Metal/Foundation).")
endif()

# ---------------------------------------------------------------------------
# Shared static core: WhisperEngine + AudioCapture + Vad + matcher
# ---------------------------------------------------------------------------
add_library(ww_core STATIC
    whisper_engine.cpp
    audio_capture.cpp
    vad.cpp
    wakeword_matcher.cpp
)
target_include_directories(ww_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../ThreadComponent
    /opt/homebrew/include
    /usr/local/include
)

set(WW_CORE_LINK
    PkgConfig::RTAUDIO
    Threads::Threads
    ${ACCELERATE_FRAMEWORK}
    ${METAL_FRAMEWORK}
    ${FOUNDATION_FRAMEWORK}
)
if(WHISPER_FOUND)
  list(APPEND WW_CORE_LINK PkgConfig::WHISPER)
else()
  list(APPEND WW_CORE_LINK ${WHISPER_LIB})
endif()
if(GGML_FOUND)
  list(APPEND WW_CORE_LINK PkgConfig::GGML)
else()
  list(APPEND WW_CORE_LINK ${GGML_LIB})
endif()

target_link_libraries(ww_core PUBLIC ${WW_CORE_LINK})

# ---------------------------------------------------------------------------
# Main wake-word listener app (default)
# ---------------------------------------------------------------------------
add_executable(${PROJECT_NAME}
    example_wakeword.cpp
    wakeWord_whisper.cpp
)
target_link_libraries(${PROJECT_NAME} PRIVATE ww_core)

# ---------------------------------------------------------------------------
# Unit tests (pure logic: resampler + VAD + matcher, no audio hardware needed)
# ---------------------------------------------------------------------------
if(BUILD_TESTING)
  enable_testing()
  add_executable(ww_tests tests/test_wakeword_utils.cpp)
  target_link_libraries(ww_tests PRIVATE ww_core)
  add_test(NAME ww_tests COMMAND ww_tests)
endif()

foreach(target ${PROJECT_NAME} ww_core ww_tests)
  if(TARGET ${target})
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endforeach()