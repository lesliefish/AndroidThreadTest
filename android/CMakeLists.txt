# Entry CMakeLists for android/ directory
# Includes the app-level CMake which defines native_bench + threadtest_jni

cmake_minimum_required(VERSION 3.18.1)
project(ThreadBenchmarkAndroid)

add_subdirectory(app)
