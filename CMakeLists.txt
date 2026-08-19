cmake_minimum_required(VERSION 3.16)

project(t113_terminal
    VERSION 0.1.0
    LANGUAGES C
)

find_package(Threads REQUIRED)
include(CTest)

# The build host may provide a newer shared libm than the T113 image.
# Link libm statically so the binary does not require a newer GLIBC symbol
# version from /lib/libm.so.6 at runtime.
set(T113_LIBM_STATIC "")
if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|aarch64)" OR
    CMAKE_C_COMPILER MATCHES "(arm|aarch64)-linux")
    execute_process(
        COMMAND ${CMAKE_C_COMPILER} -print-file-name=libm.a
        OUTPUT_VARIABLE T113_LIBM_STATIC
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(t113_terminal
    src/console_log.c
    src/main.c
    src/can_receiver.c
    src/can_decoder.c
    src/gps_track.c
    src/lap_timer.c
    src/accel_test.c
    src/race_event.c
    src/logger.c
    src/replay.c
    src/vehicle_state_snapshot.c
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

if (EXISTS "${T113_LIBM_STATIC}")
    target_link_libraries(t113_terminal PRIVATE "${T113_LIBM_STATIC}")
else()
    target_link_libraries(t113_terminal PRIVATE m)
endif()

if (BUILD_TESTING)
    add_executable(test_can_decoder
        tests/test_can_decoder.c
        src/can_decoder.c
        src/console_log.c
    )
    target_include_directories(test_can_decoder PRIVATE include)
    target_compile_options(test_can_decoder PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(test_can_decoder PRIVATE Threads::Threads)
    add_test(NAME can_decoder_freshness COMMAND test_can_decoder)

    add_executable(test_lap_timer_trace
        tests/test_lap_timer_trace.c
        src/lap_timer.c
        src/gps_track.c
        src/console_log.c
    )
    target_include_directories(test_lap_timer_trace PRIVATE include)
    target_compile_options(test_lap_timer_trace PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(test_lap_timer_trace PRIVATE
        Threads::Threads
        m
    )
    add_test(NAME lap_timer_trace_async COMMAND test_lap_timer_trace)

    add_executable(test_lap_timer_best_reference
        tests/test_lap_timer_best_reference.c
        src/lap_timer.c
        src/gps_track.c
        src/console_log.c
    )
    target_include_directories(test_lap_timer_best_reference PRIVATE include)
    target_compile_options(test_lap_timer_best_reference PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(test_lap_timer_best_reference PRIVATE
        Threads::Threads
        m
    )
    add_test(NAME lap_timer_best_reference
             COMMAND test_lap_timer_best_reference)

    add_executable(test_race_event_accel
        tests/test_race_event_accel.c
        src/race_event.c
        src/accel_test.c
        src/lap_timer.c
        src/gps_track.c
        src/console_log.c
    )
    target_include_directories(test_race_event_accel PRIVATE include)
    target_compile_options(test_race_event_accel PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(test_race_event_accel PRIVATE
        Threads::Threads
        m
    )
    add_test(NAME race_event_parallel_accel COMMAND test_race_event_accel)

    add_executable(test_race_event_laptime
        tests/test_race_event_laptime.c
        src/race_event.c
        src/accel_test.c
        src/lap_timer.c
        src/gps_track.c
        src/console_log.c
    )
    target_include_directories(test_race_event_laptime PRIVATE include)
    target_compile_options(test_race_event_laptime PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(test_race_event_laptime PRIVATE
        Threads::Threads
        m
    )
    add_test(NAME race_event_laptime COMMAND test_race_event_laptime)

    add_executable(test_accel_axis
        tests/test_accel_axis.c
        src/accel_test.c
        src/console_log.c
    )
    target_include_directories(test_accel_axis PRIVATE include)
    target_compile_options(test_accel_axis PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(test_accel_axis PRIVATE
        Threads::Threads
        m
    )
    add_test(NAME accel_axis_acquisition COMMAND test_accel_axis)

    add_executable(test_replay_vehicle_state_snapshot
        tests/test_replay_vehicle_state_snapshot.c
        src/replay.c
        src/vehicle_state_snapshot.c
        src/console_log.c
    )
    target_include_directories(test_replay_vehicle_state_snapshot PRIVATE
        include
    )
    target_compile_options(test_replay_vehicle_state_snapshot PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(test_replay_vehicle_state_snapshot PRIVATE
        Threads::Threads
        m
    )
    add_test(NAME replay_vehicle_state_snapshot
             COMMAND test_replay_vehicle_state_snapshot)
endif()
