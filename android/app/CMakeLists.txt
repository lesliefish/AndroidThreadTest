# Android NDK benchmark executable
# Built by Android Studio / ndk-build / CMake

cmake_minimum_required(VERSION 3.18.1)
project(threadtest)

set(CMAKE_C_STANDARD 11)

# Native benchmark executable
add_executable(native_bench
    app/src/main/jni/native_bench.c
)

target_include_directories(native_bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(native_bench
    log
    android
    m
    pthread
)
