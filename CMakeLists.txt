cmake_minimum_required(VERSION 3.16)
project(WhisperWakeWordEngine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Thread library dependencies setup
find_package(Threads REQUIRED)

# 2. PkgConfig configuration tracking for Homebrew RtAudio bindings
find_package(PkgConfig REQUIRED)
pkg_check_modules(RTAUDIO REQUIRED IMPORTED_TARGET rtaudio)

# 3. Discover Homebrew deployment locations for Whisper and GGML targets
find_library(WHISPER_LIB NAMES whisper HINTS /opt/homebrew/lib /usr/local/lib REQUIRED)
find_library(GGML_LIB NAMES ggml ggml-base HINTS /opt/homebrew/lib /usr/local/lib REQUIRED)

# 4. Apple Framework bindings for M1 hardware acceleration 
find_library(ACCELERATE_FRAMEWORK Accelerate REQUIRED)
find_library(METAL_FRAMEWORK Metal REQUIRED)
find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)

# 5. Build assembly output mapping targets
add_executable(${PROJECT_NAME} 
    example_wakeword.cpp 
    wakeWord_whisper.cpp
)

# 6. Expose header folder maps
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../ThreadComponent
    /opt/homebrew/include
    /usr/local/include
)

# 7. Final runtime library compilation link steps
target_link_libraries(${PROJECT_NAME} PRIVATE
    PkgConfig::RTAUDIO            
    ${WHISPER_LIB}
    ${GGML_LIB}                   
    ${ACCELERATE_FRAMEWORK}
    ${METAL_FRAMEWORK}
    ${FOUNDATION_FRAMEWORK}
    Threads::Threads              
)

target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
