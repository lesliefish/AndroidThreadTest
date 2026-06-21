cmake_minimum_required(VERSION 3.18)
project(ThreadBenchmark LANGUAGES CXX)

# ─── Global settings ───────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── Shared benchmark primitives ───────────────────────────────────
# Cross-platform core benchmark logic (prime counting, statistics, experiments)
# Platform-specific code (threading, CPU binding) is implemented separately.

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
