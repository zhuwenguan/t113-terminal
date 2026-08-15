cmake_minimum_required(VERSION 3.16)

project(t113_terminal
    VERSION 0.1.0
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(t113_terminal
    src/main.c
    src/app_config.c
    src/can_frame.c
    src/can_service.c
    src/can_socket.c
    src/signal_control.c
)

target_include_directories(t113_terminal PRIVATE
    include
)

target_compile_options(t113_terminal PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
