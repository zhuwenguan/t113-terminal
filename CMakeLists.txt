cmake_minimum_required(VERSION 3.16)

project(t113_terminal
    VERSION 0.1.0
    LANGUAGES C
)

find_package(Threads REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(t113_terminal
    src/console_log.c
    src/main.c
    src/can_receiver.c
    src/logger.c
    src/lappro.c
    src/lappro_transport.c
    src/lappro_crypto.c
)

target_include_directories(t113_terminal PRIVATE
    include
)

target_compile_options(t113_terminal PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_link_libraries(t113_terminal PRIVATE
    Threads::Threads
)
