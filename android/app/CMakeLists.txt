# Android NDK benchmark (C++11) — builds both:
#   1) native_bench      : standalone ARM64 executable (for adb shell)
#   2) libthreadtest_jni : shared library (for APK JNI bridge)
#
# 使用共享的 bench_core 实现跨平台基准测试逻辑。

cmake_minimum_required(VERSION 3.18.1)
project(threadtest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Standalone executable ─────────────────────────────────────────
add_executable(native_bench
    src/main/jni/native_bench_main.cpp
    ../../../shared/bench_core.cpp
)

target_include_directories(native_bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(native_bench
    log android m pthread
)

# ── JNI shared library ───────────────────────────────────────────
add_library(threadtest_jni SHARED
    src/main/jni/jni_bridge.cpp
)

target_include_directories(threadtest_jni PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main/jni
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(threadtest_jni
    log android m pthread
)