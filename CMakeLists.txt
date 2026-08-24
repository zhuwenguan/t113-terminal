cmake_minimum_required(VERSION 3.16)

project(t113_terminal
    VERSION 0.1.0
    LANGUAGES C
)

add_executable(g2d_smoke_test
    tools/g2d_smoke_test.c
)

add_executable(vencoder_overlay_test
    tools/vencoder_overlay_test.c
)

add_executable(vencoder_rgb_test
    tools/vencoder_rgb_test.c
)

add_executable(video_argb_pipeline_test
    tools/video_argb_pipeline_test.c
    src/hud_renderer.c
)

add_executable(hud_ion_test
    tools/hud_ion_test.c
    src/hud_renderer.c
)

add_executable(mjpeg_mp4_inspect
    tools/mjpeg_mp4_inspect.c
)

target_include_directories(g2d_smoke_test PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_include_directories(vencoder_overlay_test PRIVATE
    include
)

target_include_directories(vencoder_rgb_test PRIVATE
    include
)

target_include_directories(video_argb_pipeline_test PRIVATE
    ${CMAKE_SOURCE_DIR}
    include
)

target_include_directories(hud_ion_test PRIVATE
    ${CMAKE_SOURCE_DIR}
    include
)

target_compile_options(g2d_smoke_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_compile_options(vencoder_overlay_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_compile_options(vencoder_rgb_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_compile_options(video_argb_pipeline_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_compile_options(hud_ion_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_compile_options(mjpeg_mp4_inspect PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_link_libraries(g2d_smoke_test PRIVATE
    ${CMAKE_DL_LIBS}
)

target_link_libraries(vencoder_overlay_test PRIVATE
    ${CMAKE_DL_LIBS}
)

target_link_libraries(vencoder_rgb_test PRIVATE
    ${CMAKE_DL_LIBS}
)

target_link_libraries(video_argb_pipeline_test PRIVATE
    ${CMAKE_DL_LIBS}
    m
)

target_link_libraries(hud_ion_test PRIVATE
    ${CMAKE_DL_LIBS}
    m
)

find_path(T113_JPEG_INCLUDE_DIR jpeglib.h
    PATHS
        ${CMAKE_SYSROOT}/usr/include
        ${CMAKE_SYSROOT}/include
        ${CMAKE_SOURCE_DIR}/third_party/libjpeg
)
if (NOT T113_JPEG_INCLUDE_DIR)
    set(T113_JPEG_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/libjpeg)
endif()

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
    src/vehicle_state_clock.c
    src/vehicle_state_snapshot.c
    src/hmi.c
    src/video_frame.c
    src/encoded_frame_queue.c
    src/jpeg_codec.c
    src/video_encoder.c
    src/video_capture.c
    src/hud_renderer.c
    src/video_compositor.c
    src/video_recorder.c
    src/video_pipeline.c
    src/lappro.c
    src/lappro_transport.c
    src/lappro_crypto.c
)

target_include_directories(t113_terminal PRIVATE
    ${CMAKE_SOURCE_DIR}
    include
    ${T113_JPEG_INCLUDE_DIR}
)

target_compile_options(t113_terminal PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_link_libraries(t113_terminal PRIVATE
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|aarch64)" OR
    CMAKE_C_COMPILER MATCHES "(arm|aarch64)-linux")
    target_compile_options(t113_terminal PRIVATE
        -mfpu=neon
        -mfloat-abi=hard
    )
endif()

if (EXISTS "${T113_LIBM_STATIC}")
    target_link_libraries(t113_terminal PRIVATE "${T113_LIBM_STATIC}")
else()
    target_link_libraries(t113_terminal PRIVATE m)
endif()

if (BUILD_TESTING)
    add_library(video_writer_fault_preload SHARED
        tests/video_writer_fault_preload.c
    )
    target_compile_options(video_writer_fault_preload PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(video_writer_fault_preload PRIVATE
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )

    add_executable(test_vehicle_state_clock
        tests/test_vehicle_state_clock.c
        src/vehicle_state_clock.c
    )
    target_include_directories(test_vehicle_state_clock PRIVATE include)
    target_compile_options(test_vehicle_state_clock PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    add_test(NAME vehicle_state_live_replay_clock_and_gps_freshness
             COMMAND test_vehicle_state_clock)

    add_executable(test_encoded_frame_queue
        tests/test_encoded_frame_queue.c
        src/encoded_frame_queue.c
    )
    target_include_directories(test_encoded_frame_queue PRIVATE include)
    target_compile_options(test_encoded_frame_queue PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(test_encoded_frame_queue PRIVATE Threads::Threads)
    add_test(NAME encoded_frame_queue_bounded_lifecycle
             COMMAND test_encoded_frame_queue)

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

    add_executable(test_hmi
        tests/test_hmi.c
        src/hmi.c
        src/console_log.c
    )
    target_include_directories(test_hmi PRIVATE include)
    target_compile_options(test_hmi PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(test_hmi PRIVATE
        Threads::Threads
        m
    )
    add_test(NAME hmi_ascii_rs485_protocol COMMAND test_hmi)

    add_executable(test_hud_renderer
        tests/test_hud_renderer.c
        src/hud_renderer.c
        src/video_compositor.c
        src/video_frame.c
    )
    target_include_directories(test_hud_renderer PRIVATE
        ${CMAKE_SOURCE_DIR}
        include
    )
    target_compile_options(test_hud_renderer PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(test_hud_renderer PRIVATE m ${CMAKE_DL_LIBS})
    add_test(NAME hud_renderer_layout_and_invalid_state
             COMMAND test_hud_renderer)

    add_executable(test_video_recorder_timestamps
        tests/test_video_recorder_timestamps.c
        src/encoded_frame_queue.c
        src/video_recorder.c
        src/video_encoder.c
        src/jpeg_codec.c
        src/video_frame.c
        src/console_log.c
    )
    target_include_directories(test_video_recorder_timestamps PRIVATE
        include
        ${T113_JPEG_INCLUDE_DIR}
    )
    target_compile_options(test_video_recorder_timestamps PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(test_video_recorder_timestamps PRIVATE
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )
    add_test(NAME video_recorder_variable_timestamps
             COMMAND test_video_recorder_timestamps)

    add_executable(test_jpeg_codec
        tests/test_jpeg_codec.c
        src/jpeg_codec.c
        src/video_frame.c
        src/console_log.c
    )
    target_include_directories(test_jpeg_codec PRIVATE
        include
        ${T113_JPEG_INCLUDE_DIR}
    )
    target_compile_options(test_jpeg_codec PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(test_jpeg_codec PRIVATE ${CMAKE_DL_LIBS})
endif()
