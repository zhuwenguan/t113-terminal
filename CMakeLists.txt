cmake_minimum_required(VERSION 3.16)

project(t113_terminal
    VERSION 0.1.0
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(t113_terminal
    src/main.c
)

target_compile_options(t113_terminal PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
