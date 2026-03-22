cmake_minimum_required(VERSION 3.22.1)
project(emu7800 C CXX)

# SDL2: build from source
# Download SDL2-2.30.x and extract into app/src/main/jni/SDL2/
set(SDL2_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/SDL2)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
add_subdirectory(${SDL2_SOURCE_DIR} SDL2-build EXCLUDE_FROM_ALL)

# SDL2 Android Java support files are in SDL2/android-project/app/src/main/java
# They must be copied to the app/src/main/java directory or referenced via sourceSets.

set(CORE_SRCS
    src/core/m6502.c
    src/core/tia.c
    src/core/maria.c
    src/core/pia.c
    src/core/cart.c
    src/core/machine.c
    src/core/tiasound.c
    src/core/pokeysound.c
    src/core/savestate.c
)

set(PLATFORM_SRCS
    src/platform/main.c
    src/platform/video.c
    src/platform/audio.c
    src/platform/input.c
    src/platform/font.c
    src/platform/filepicker.c
    src/platform/jni_bridge.c
    src/platform/log_android.c
)

add_library(main SHARED ${CORE_SRCS} ${PLATFORM_SRCS})

target_include_directories(main PRIVATE
    src/core
    src/platform
    ${SDL2_SOURCE_DIR}/include
)

target_compile_options(main PRIVATE
    -std=c99
    -O2
    -Wall
    -Wno-unused-parameter
    -Wno-missing-field-initializers
)

target_compile_definitions(main PRIVATE ANDROID)

if(DEFINED EMU7800_VERSION)
    target_compile_definitions(main PRIVATE "EMU7800_VERSION=\"${EMU7800_VERSION}\"")
else()
    target_compile_definitions(main PRIVATE "EMU7800_VERSION=\"dev\"")
endif()

target_link_libraries(main
    SDL2-static
    android      # ANativeActivity, AAssetManager
    log          # __android_log_print
    OpenSLES     # SDL2 Android audio backend
    EGL
    GLESv2
)

# Align ELF LOAD segments to 16 KB so the .so passes the 16 KB page-size
# compatibility check on devices with 16 KB memory pages (required by Android
# for apps targeting API 35+ and recommended for all apps going forward).
target_link_options(main PRIVATE -Wl,-z,max-page-size=16384)
