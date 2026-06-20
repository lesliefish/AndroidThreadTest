cmake_minimum_required(VERSION 3.18)
project(ThreadBenchmark LANGUAGES C)

# ─── Global settings ───────────────────────────────────────────────
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ─── Shared benchmark primitives ───────────────────────────────────
# These are inline C functions included by both platform implementations.
# They contain the pure computation logic (prime counting, etc.)
# with no platform-specific threading calls.

# ─── Platform: Windows ─────────────────────────────────────────────
option(BUILD_WINDOWS "Build Windows benchmark" ON)
if(BUILD_WINDOWS)
    add_subdirectory(windows)
endif()

# ─── Platform: Android ─────────────────────────────────────────────
option(BUILD_ANDROID "Build Android benchmark" OFF)
if(BUILD_ANDROID)
    add_subdirectory(android)
endif()
