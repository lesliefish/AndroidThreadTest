# Windows benchmark executable (C++11)

cmake_minimum_required(VERSION 3.15)
project(ThreadTestWindows LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(threadtest
    src/native_bench.cpp
    ../shared/bench_core.cpp
)

target_include_directories(threadtest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(threadtest PRIVATE
    kernel32
    user32
)

# Performance optimization flags
if(MSVC)
    target_compile_options(threadtest PRIVATE /O2 /DNDEBUG)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(threadtest PRIVATE -O3 -DNDEBUG)
endif()
