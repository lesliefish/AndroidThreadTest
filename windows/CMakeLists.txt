# Windows benchmark executable

cmake_minimum_required(VERSION 3.15)
project(ThreadTestWindows LANGUAGES C)

set(CMAKE_C_STANDARD 11)

add_executable(threadtest
    src/native_bench.c
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
elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(threadtest PRIVATE -O3 -DNDEBUG)
endif()
