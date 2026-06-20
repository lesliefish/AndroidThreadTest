# Android NDK benchmark — builds both:
#   1) native_bench      : standalone ARM64 executable (for adb shell)
#   2) libthreadtest_jni : shared library (for APK JNI bridge)

cmake_minimum_required(VERSION 3.18.1)
project(threadtest)

set(CMAKE_C_STANDARD 11)

# ── Standalone executable ─────────────────────────────────────────
add_executable(native_bench
    app/src/main/jni/native_bench.c
)

target_include_directories(native_bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(native_bench
    log android m pthread
)

# ── JNI shared library ───────────────────────────────────────────
add_library(threadtest_jni SHARED
    app/src/main/jni/jni_bridge.c
)

target_include_directories(threadtest_jni PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/app/src/main/jni
)

target_link_libraries(threadtest_jni
    log android m pthread
)
