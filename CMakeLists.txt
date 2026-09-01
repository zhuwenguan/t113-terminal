cmake_minimum_required(VERSION 3.16)

project(t113_terminal
    VERSION 0.1.0
    LANGUAGES C
)

# The VehicleState ABI is authoritative here for every daemon target.
set(RACE_COMMON_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/libs/common/include)
set(RACE_COMMON_LEGACY_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/libs/common/include/race)
set(RACE_IPC_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/libs/ipc/include)
set(RACE_VEHICLE_STATE_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/libs/vehicle-state/include)
set(RACE_CORE_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/apps/terminal-core/include)
set(RACE_VIDEO_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/apps/terminal-core/video/include)
set(RACE_TERMINAL_CORE_SOURCE_DIR ${CMAKE_SOURCE_DIR}/apps/terminal-core/src)
set(RACE_TERMINAL_CORE_DIR ${CMAKE_SOURCE_DIR}/apps/terminal-core)
set(RACE_VIDEO_DIR ${CMAKE_SOURCE_DIR}/apps/terminal-core/video)
set(RACE_VIDEO_SOURCE_DIR ${CMAKE_SOURCE_DIR}/apps/terminal-core/video/src)
set(RACE_HUD_DIR ${CMAKE_SOURCE_DIR}/apps/terminal-core/hud)
set(RACE_UI_DIR ${CMAKE_SOURCE_DIR}/apps/terminal-core/ui)
set(RACE_WEBD_DIR ${CMAKE_SOURCE_DIR}/apps/webd)
set(RACE_CIVETWEB_DIR ${CMAKE_SOURCE_DIR}/third_party/civetweb)
set(T113_HUD_SOURCES
    ${RACE_VIDEO_SOURCE_DIR}/hud_renderer.c
    ${RACE_VIDEO_SOURCE_DIR}/hud_blend.c
    ${RACE_VIDEO_SOURCE_DIR}/hud_text.c
    ${RACE_UI_DIR}/ui_draw.c
    ${RACE_UI_DIR}/ui_font.c
    ${RACE_UI_DIR}/ui_image.c
    third_party/tiny_png.c
    ${RACE_HUD_DIR}/hud_widget.c
    ${RACE_HUD_DIR}/hud_render.c)

include_directories(
    ${RACE_COMMON_INCLUDE_DIR}
    ${RACE_IPC_INCLUDE_DIR}
    ${RACE_VEHICLE_STATE_INCLUDE_DIR}
)
include_directories(${CMAKE_SOURCE_DIR}/third_party)

add_library(ui_draw STATIC
    ${RACE_UI_DIR}/ui_draw.c
)
target_include_directories(ui_draw PUBLIC ${RACE_UI_DIR})
target_compile_features(ui_draw PUBLIC c_std_11)
target_compile_options(ui_draw PRIVATE -Wall -Wextra -Wpedantic)

add_library(ui_image STATIC
    ${RACE_UI_DIR}/ui_image.c
    third_party/tiny_png.c
)
target_include_directories(ui_image PUBLIC
    ${RACE_UI_DIR}
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_features(ui_image PUBLIC c_std_11)
target_compile_options(ui_image PRIVATE -Wall -Wextra -Wpedantic)

# FreeType is a runtime dependency of the production UI HUD.  Keep the
# locations overridable for the T113 SDK/sysroot, while making the dependency
# explicit instead of silently compiling another font implementation.
set(T113_FREETYPE_INCLUDE_DIR "" CACHE PATH
    "FreeType include directory containing ft2build.h")
set(T113_FREETYPE_LIBRARY "" CACHE FILEPATH
    "FreeType library for the selected target architecture")
set(T113_FREETYPE_DEPENDENCIES "" CACHE STRING
    "Additional libraries required by the selected FreeType build")
set(_t113_arm_target FALSE)
if (T113_ARM_TARGET OR CMAKE_SYSTEM_PROCESSOR MATCHES "^[Aa][Rr][Mm]")
    set(_t113_arm_target TRUE)
endif()

# The board's FreeType runtime is staged outside the repository for cross
# builds.  Allow that cache to be selected by an environment variable, and
# use the conventional per-user cache location when it exists.  This keeps
# developer-specific paths out of tracked presets while making the standard
# ARM presets usable on the development host.
set(T113_FREETYPE_ROOT "" CACHE PATH
    "Optional staged FreeType root containing include/ and lib/")
if (_t113_arm_target AND NOT T113_FREETYPE_ROOT)
    if (DEFINED ENV{T113_FREETYPE_ROOT} AND NOT "$ENV{T113_FREETYPE_ROOT}" STREQUAL "")
        set(T113_FREETYPE_ROOT "$ENV{T113_FREETYPE_ROOT}" CACHE PATH
            "Optional staged FreeType root containing include/ and lib/" FORCE)
    elseif (DEFINED ENV{XDG_CACHE_HOME} AND
            EXISTS "$ENV{XDG_CACHE_HOME}/t113-terminal/freetype-armhf")
        set(T113_FREETYPE_ROOT
            "$ENV{XDG_CACHE_HOME}/t113-terminal/freetype-armhf" CACHE PATH
            "Optional staged FreeType root containing include/ and lib/" FORCE)
    elseif (DEFINED ENV{HOME} AND
            EXISTS "$ENV{HOME}/.cache/t113-terminal/freetype-armhf")
        set(T113_FREETYPE_ROOT
            "$ENV{HOME}/.cache/t113-terminal/freetype-armhf" CACHE PATH
            "Optional staged FreeType root containing include/ and lib/" FORCE)
    endif()
endif()
if (_t113_arm_target AND T113_FREETYPE_ROOT)
    if (NOT T113_FREETYPE_INCLUDE_DIR)
        foreach(_t113_freetype_include_candidate
                "${T113_FREETYPE_ROOT}/include/usr/include/freetype2"
                "${T113_FREETYPE_ROOT}/include/freetype2"
                "${T113_FREETYPE_ROOT}/usr/include/freetype2")
            if (EXISTS "${_t113_freetype_include_candidate}/ft2build.h")
                set(T113_FREETYPE_INCLUDE_DIR
                    "${_t113_freetype_include_candidate}" CACHE PATH
                    "FreeType include directory containing ft2build.h" FORCE)
                break()
            endif()
        endforeach()
    endif()
    if (NOT T113_FREETYPE_LIBRARY)
        foreach(_t113_freetype_library_candidate
                "${T113_FREETYPE_ROOT}/lib/libfreetype.so.6.16.1"
                "${T113_FREETYPE_ROOT}/lib/libfreetype.so.6"
                "${T113_FREETYPE_ROOT}/lib/libfreetype.so"
                "${T113_FREETYPE_ROOT}/usr/lib/libfreetype.so.6.16.1"
                "${T113_FREETYPE_ROOT}/usr/lib/libfreetype.so.6"
                "${T113_FREETYPE_ROOT}/usr/lib/libfreetype.so")
            if (EXISTS "${_t113_freetype_library_candidate}")
                set(T113_FREETYPE_LIBRARY
                    "${_t113_freetype_library_candidate}" CACHE FILEPATH
                    "FreeType library for the selected target architecture"
                    FORCE)
                break()
            endif()
        endforeach()
    endif()
    if (NOT T113_FREETYPE_DEPENDENCIES)
        set(_t113_freetype_dependencies)
        foreach(_t113_freetype_dependency_candidate
                "${T113_FREETYPE_ROOT}/lib/libpng16.so.16.36.0"
                "${T113_FREETYPE_ROOT}/lib/libpng16.so.16"
                "${T113_FREETYPE_ROOT}/lib/libpng16.so"
                "${T113_FREETYPE_ROOT}/usr/lib/libpng16.so.16.36.0"
                "${T113_FREETYPE_ROOT}/usr/lib/libpng16.so.16"
                "${T113_FREETYPE_ROOT}/usr/lib/libpng16.so")
            if (EXISTS "${_t113_freetype_dependency_candidate}")
                list(APPEND _t113_freetype_dependencies
                    "${_t113_freetype_dependency_candidate}")
                break()
            endif()
        endforeach()
        foreach(_t113_freetype_dependency_candidate
                "${T113_FREETYPE_ROOT}/lib/libz.so.1.2.11"
                "${T113_FREETYPE_ROOT}/lib/libz.so.1"
                "${T113_FREETYPE_ROOT}/lib/libz.so"
                "${T113_FREETYPE_ROOT}/usr/lib/libz.so.1.2.11"
                "${T113_FREETYPE_ROOT}/usr/lib/libz.so.1"
                "${T113_FREETYPE_ROOT}/usr/lib/libz.so")
            if (EXISTS "${_t113_freetype_dependency_candidate}")
                list(APPEND _t113_freetype_dependencies
                    "${_t113_freetype_dependency_candidate}")
                break()
            endif()
        endforeach()
        if (_t113_freetype_dependencies)
            set(T113_FREETYPE_DEPENDENCIES
                "${_t113_freetype_dependencies}" CACHE STRING
                "Additional libraries required by the selected FreeType build"
                FORCE)
        endif()
        unset(_t113_freetype_dependencies)
    endif()
endif()
if (NOT T113_FREETYPE_INCLUDE_DIR)
    find_path(T113_FREETYPE_INCLUDE_DIR ft2build.h
        PATH_SUFFIXES freetype2)
endif()
if (NOT T113_FREETYPE_LIBRARY)
    find_library(T113_FREETYPE_LIBRARY
        NAMES freetype freetype.so.6 libfreetype.so.6)
endif()
if (NOT T113_FREETYPE_INCLUDE_DIR OR NOT T113_FREETYPE_LIBRARY)
    message(FATAL_ERROR
        "FreeType development files are required for the T113 HUD. "
        "Set T113_FREETYPE_INCLUDE_DIR and T113_FREETYPE_LIBRARY for the "
        "host or T113 sysroot.")
endif()
add_library(t113_freetype INTERFACE)
target_include_directories(t113_freetype INTERFACE
    ${T113_FREETYPE_INCLUDE_DIR})
target_link_libraries(t113_freetype INTERFACE
    ${T113_FREETYPE_LIBRARY} ${T113_FREETYPE_DEPENDENCIES} m)

add_library(ui_font STATIC
    ${RACE_UI_DIR}/ui_font.c
    ${RACE_VIDEO_SOURCE_DIR}/hud_text.c
    ${RACE_VIDEO_SOURCE_DIR}/hud_blend.c
)
target_include_directories(ui_font PUBLIC
    ${RACE_UI_DIR}
    ${RACE_HUD_DIR}
    ${RACE_VIDEO_INCLUDE_DIR}
)
target_compile_features(ui_font PUBLIC c_std_11)
target_compile_options(ui_font PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(ui_font PUBLIC t113_freetype)

add_library(hud_widget STATIC
    ${RACE_HUD_DIR}/hud_widget.c
)
target_include_directories(hud_widget PUBLIC
    ${RACE_HUD_DIR}
    ${RACE_UI_DIR}
)
target_compile_features(hud_widget PUBLIC c_std_11)
target_compile_options(hud_widget PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(hud_widget PUBLIC ui_draw ui_font)

add_executable(g2d_smoke_test
    tools/g2d_smoke_test.c
)

add_executable(vencoder_overlay_test
    tools/vencoder_overlay_test.c
)

add_executable(vencoder_rgb_test
    tools/vencoder_rgb_test.c
)

add_executable(mjpeg_mp4_inspect
    tools/mjpeg_mp4_inspect.c
)

target_include_directories(g2d_smoke_test PRIVATE
    ${RACE_VIDEO_INCLUDE_DIR}
)

target_include_directories(vencoder_overlay_test PRIVATE
    ${RACE_VIDEO_INCLUDE_DIR}
)

target_include_directories(vencoder_rgb_test PRIVATE
    ${RACE_VIDEO_INCLUDE_DIR}
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

add_subdirectory(libs/ipc)
add_subdirectory(libs/common)

# CivetWeb is embedded as a fixed, source-vendored dependency so the web
# daemon remains self-contained on the T113 image.  Only HTTP, static files
# and WebSocket support are enabled; TLS/CGI/Lua/compression are intentionally
# outside this local Phase 1 boundary.
add_library(civetweb STATIC
    ${RACE_CIVETWEB_DIR}/src/civetweb.c
)
target_include_directories(civetweb PUBLIC
    ${RACE_CIVETWEB_DIR}/include
)
target_compile_definitions(civetweb PUBLIC
    USE_WEBSOCKET
    NO_SSL
    NO_CGI
    NO_LUA
    NO_DUKTAPE
    NO_ZLIB
    NO_HTTP2
)
target_compile_features(civetweb PUBLIC c_std_11)
target_compile_options(civetweb PRIVATE -w)
target_link_libraries(civetweb PUBLIC Threads::Threads)

add_executable(mjpeg_mp4_recover
    tools/mjpeg_mp4_recover.c
    ${RACE_VIDEO_SOURCE_DIR}/encoded_frame_queue.c
    ${RACE_VIDEO_SOURCE_DIR}/video_recorder.c
    ${RACE_VIDEO_SOURCE_DIR}/video_encoder.c
    ${RACE_VIDEO_SOURCE_DIR}/jpeg_codec.c
    ${RACE_VIDEO_SOURCE_DIR}/video_frame.c
)
target_include_directories(mjpeg_mp4_recover PRIVATE
    ${RACE_COMMON_LEGACY_INCLUDE_DIR}
    ${RACE_VIDEO_INCLUDE_DIR}
    ${T113_JPEG_INCLUDE_DIR}
)
target_compile_options(mjpeg_mp4_recover PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(mjpeg_mp4_recover PRIVATE
    race_common Threads::Threads ${CMAKE_DL_LIBS}
)

add_executable(race_state_dump tools/race_state_dump.c)
target_link_libraries(race_state_dump PRIVATE race_common race_ipc)

add_executable(webd
    ${RACE_WEBD_DIR}/src/main.c
    ${RACE_WEBD_DIR}/src/state_reader.c
    ${RACE_WEBD_DIR}/src/webd_config.c
    ${RACE_WEBD_DIR}/src/webd_json.c
)
target_include_directories(webd PRIVATE
    ${RACE_WEBD_DIR}/include
)
target_compile_definitions(webd PRIVATE USE_WEBSOCKET)
target_compile_options(webd PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(webd PRIVATE race_common race_ipc civetweb Threads::Threads)

add_executable(fake_state_producer tools/fake_state_producer.c)
target_compile_options(fake_state_producer PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(fake_state_producer PRIVATE race_common race_ipc Threads::Threads m)

add_executable(t113_terminal
    ${RACE_TERMINAL_CORE_DIR}/main.c
    ${RACE_TERMINAL_CORE_SOURCE_DIR}/can_receiver.c
    ${RACE_TERMINAL_CORE_SOURCE_DIR}/can_decoder.c
    ${RACE_TERMINAL_CORE_SOURCE_DIR}/gps_track.c
    ${RACE_TERMINAL_CORE_SOURCE_DIR}/lap_timer.c
    ${RACE_TERMINAL_CORE_SOURCE_DIR}/accel_test.c
    ${RACE_TERMINAL_CORE_SOURCE_DIR}/race_event.c
    ${RACE_TERMINAL_CORE_SOURCE_DIR}/logger.c
    ${RACE_TERMINAL_CORE_SOURCE_DIR}/replay.c
    ${RACE_TERMINAL_CORE_SOURCE_DIR}/vehicle_state_clock.c
    ${RACE_TERMINAL_CORE_SOURCE_DIR}/vehicle_state_snapshot.c
    ${RACE_TERMINAL_CORE_SOURCE_DIR}/hmi.c
    ${RACE_VIDEO_SOURCE_DIR}/video_frame.c
    ${RACE_VIDEO_SOURCE_DIR}/encoded_frame_queue.c
    ${RACE_VIDEO_SOURCE_DIR}/jpeg_codec.c
    ${RACE_VIDEO_SOURCE_DIR}/video_encoder.c
    ${RACE_VIDEO_SOURCE_DIR}/video_capture.c
    ${T113_HUD_SOURCES}
    ${RACE_VIDEO_SOURCE_DIR}/video_compositor.c
    ${RACE_VIDEO_SOURCE_DIR}/video_recorder.c
    ${RACE_VIDEO_SOURCE_DIR}/video_pipeline.c
    ${RACE_TERMINAL_CORE_SOURCE_DIR}/lappro.c
    ${RACE_TERMINAL_CORE_SOURCE_DIR}/lappro_transport.c
    ${RACE_TERMINAL_CORE_SOURCE_DIR}/lappro_crypto.c
)

target_include_directories(t113_terminal PRIVATE
    ${RACE_COMMON_LEGACY_INCLUDE_DIR}
    ${RACE_CORE_INCLUDE_DIR}
    ${RACE_VIDEO_INCLUDE_DIR}
    ${RACE_UI_DIR}
    ${RACE_HUD_DIR}
    ${T113_JPEG_INCLUDE_DIR}
)

target_compile_options(t113_terminal PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_link_libraries(t113_terminal PRIVATE
    race_common
    race_ipc
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
target_link_libraries(t113_terminal PRIVATE t113_freetype)

# Phase 1 daemons. race_core deliberately reuses the validated vehicle-data
# sources, but it must not even link camera/G2D/encoder code: video is a real
# process fault boundary, not merely a runtime command-line distinction.
set(RACE_CORE_SOURCES
    ${RACE_TERMINAL_CORE_DIR}/main.c ${RACE_TERMINAL_CORE_SOURCE_DIR}/can_receiver.c
    ${RACE_TERMINAL_CORE_SOURCE_DIR}/can_decoder.c ${RACE_TERMINAL_CORE_SOURCE_DIR}/gps_track.c ${RACE_TERMINAL_CORE_SOURCE_DIR}/lap_timer.c
    ${RACE_TERMINAL_CORE_SOURCE_DIR}/accel_test.c ${RACE_TERMINAL_CORE_SOURCE_DIR}/race_event.c ${RACE_TERMINAL_CORE_SOURCE_DIR}/logger.c
    ${RACE_TERMINAL_CORE_SOURCE_DIR}/replay.c ${RACE_TERMINAL_CORE_SOURCE_DIR}/vehicle_state_clock.c
    ${RACE_TERMINAL_CORE_SOURCE_DIR}/vehicle_state_snapshot.c ${RACE_TERMINAL_CORE_SOURCE_DIR}/hmi.c ${RACE_TERMINAL_CORE_SOURCE_DIR}/lappro.c
    ${RACE_TERMINAL_CORE_SOURCE_DIR}/lappro_transport.c ${RACE_TERMINAL_CORE_SOURCE_DIR}/lappro_crypto.c)
add_executable(race_core ${RACE_CORE_SOURCES})
target_compile_definitions(race_core PRIVATE RACE_CORE_DAEMON=1)
target_include_directories(race_core PRIVATE
    ${RACE_COMMON_LEGACY_INCLUDE_DIR} ${RACE_CORE_INCLUDE_DIR}
    ${T113_JPEG_INCLUDE_DIR})
target_compile_options(race_core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(race_core PRIVATE race_common race_ipc Threads::Threads ${CMAKE_DL_LIBS})

set(RACE_VIDEO_SOURCES
    ${RACE_VIDEO_DIR}/main.c
    ${RACE_VIDEO_SOURCE_DIR}/video_frame.c
    ${RACE_VIDEO_SOURCE_DIR}/encoded_frame_queue.c ${RACE_VIDEO_SOURCE_DIR}/jpeg_codec.c
    ${RACE_VIDEO_SOURCE_DIR}/video_encoder.c ${RACE_VIDEO_SOURCE_DIR}/video_capture.c
    ${T113_HUD_SOURCES} ${RACE_VIDEO_SOURCE_DIR}/video_compositor.c
    ${RACE_VIDEO_SOURCE_DIR}/video_recorder.c ${RACE_VIDEO_SOURCE_DIR}/video_pipeline.c)
add_executable(race_video ${RACE_VIDEO_SOURCES})
target_include_directories(race_video PRIVATE
    ${RACE_COMMON_LEGACY_INCLUDE_DIR} ${RACE_VIDEO_INCLUDE_DIR}
    ${RACE_UI_DIR} ${RACE_HUD_DIR}
    ${T113_JPEG_INCLUDE_DIR})
target_compile_options(race_video PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(race_video PRIVATE race_common race_ipc Threads::Threads ${CMAKE_DL_LIBS})
target_link_libraries(race_video PRIVATE t113_freetype)

if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|aarch64)" OR
    CMAKE_C_COMPILER MATCHES "(arm|aarch64)-linux")
    target_compile_options(race_core PRIVATE -mfpu=neon -mfloat-abi=hard)
    target_compile_options(race_video PRIVATE -mfpu=neon -mfloat-abi=hard)
endif()
if (EXISTS "${T113_LIBM_STATIC}")
    target_link_libraries(race_core PRIVATE "${T113_LIBM_STATIC}")
    target_link_libraries(race_video PRIVATE "${T113_LIBM_STATIC}")
else()
    target_link_libraries(race_core PRIVATE m)
    target_link_libraries(race_video PRIVATE m)
endif()

install(TARGETS race_core race_video race_state_dump webd RUNTIME DESTINATION bin)
install(TARGETS race_common ARCHIVE DESTINATION lib)
install(TARGETS race_ipc ARCHIVE DESTINATION lib)
install(DIRECTORY libs/common/include/race DESTINATION include)
install(DIRECTORY libs/ipc/include/race DESTINATION include)
install(DIRECTORY libs/protocol/include/race DESTINATION include)
install(DIRECTORY libs/vehicle-state/include/race DESTINATION include)
install(DIRECTORY configs/t113/ DESTINATION config)
install(DIRECTORY web/dashboard/dist/ DESTINATION web)
install(DIRECTORY assets/fonts DESTINATION share/t113-terminal/assets)
install(FILES
    assets/icons/car-battery.png
    assets/icons/timer.png
    assets/icons/lightning.png
    assets/icons/engine.png
    assets/icons/cpu.png
    assets/icons/battery-charging-vertical.png
    assets/icons/flag-checkered.png
    DESTINATION share/t113-terminal/assets/icons)

if (BUILD_TESTING)
    add_executable(test_ui_draw tests/test_ui_draw.c)
    target_include_directories(test_ui_draw PRIVATE
        ${RACE_UI_DIR}
        ${RACE_VIDEO_INCLUDE_DIR}
        ${RACE_HUD_DIR}
    )
    target_compile_options(test_ui_draw PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_ui_draw PRIVATE ui_draw)
    add_test(NAME ui_draw_geometry_clipping_stride_and_guards
             COMMAND test_ui_draw)

    add_executable(test_ui_image tests/test_ui_image.c)
    target_include_directories(test_ui_image PRIVATE ${RACE_UI_DIR})
    target_compile_options(test_ui_image PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_ui_image PRIVATE ui_image)
    add_test(NAME ui_image_load_binary_alpha_clipping_and_guards
             COMMAND test_ui_image
                     ${CMAKE_SOURCE_DIR}/assets/icons/car-battery.png
                     ${CMAKE_SOURCE_DIR}/assets/icons/timer.png
                     ${CMAKE_SOURCE_DIR}/assets/icons/lightning.png
                     ${CMAKE_SOURCE_DIR}/assets/icons/engine.png
                     ${CMAKE_SOURCE_DIR}/assets/icons/cpu.png
                     ${CMAKE_SOURCE_DIR}/assets/icons/battery-charging-vertical.png
                     ${CMAKE_SOURCE_DIR}/assets/icons/flag-checkered.png)

    add_executable(test_ui_font tests/test_ui_font.c)
    target_include_directories(test_ui_font PRIVATE ${RACE_UI_DIR})
    target_compile_options(test_ui_font PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_ui_font PRIVATE ui_font)
    add_test(NAME ui_font_measure_draw_clipping_and_lifecycle
             COMMAND test_ui_font ${CMAKE_SOURCE_DIR}/assets/fonts/hud.ttf)

    add_executable(test_ui_phase2 tests/test_ui_phase2.c)
    target_include_directories(test_ui_phase2 PRIVATE
        ${RACE_UI_DIR}
        ${RACE_VIDEO_INCLUDE_DIR}
    )
    target_compile_options(test_ui_phase2 PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_ui_phase2 PRIVATE ui_draw ui_font ui_image)
    add_test(NAME ui_phase2_geometry_font_and_icon_smoke
             COMMAND test_ui_phase2
                     ${CMAKE_SOURCE_DIR}/assets/fonts/hud.ttf)

    add_executable(test_hud_widget
        tests/test_hud_widget.c
        ${RACE_HUD_DIR}/hud_widget.c
    )
    target_include_directories(test_hud_widget PRIVATE
        ${RACE_HUD_DIR}
        ${RACE_UI_DIR}
    )
    target_compile_options(test_hud_widget PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_hud_widget PRIVATE ui_draw)
    add_test(NAME hud_widget_composition_clipping_stride_and_guards
             COMMAND test_hud_widget)

    add_executable(test_hud_render
        tests/test_hud_render.c
        ${RACE_HUD_DIR}/hud_render.c
        ${RACE_HUD_DIR}/hud_widget.c
    )
    target_include_directories(test_hud_render PRIVATE
        ${RACE_HUD_DIR}
        ${RACE_UI_DIR}
        ${RACE_COMMON_INCLUDE_DIR}
        ${RACE_VIDEO_INCLUDE_DIR}
    )
    target_compile_options(test_hud_render PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_hud_render PRIVATE ui_draw ui_image m)
    add_test(NAME hud_render_static_state_layout_guard_stride
             COMMAND test_hud_render)
    set_tests_properties(hud_render_static_state_layout_guard_stride
                         PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    add_executable(hud_perf_benchmark
        tests/hud_perf_benchmark.c
        ${RACE_HUD_DIR}/hud_render.c
        ${RACE_HUD_DIR}/hud_widget.c
    )
    target_include_directories(hud_perf_benchmark PRIVATE
        ${RACE_HUD_DIR}
        ${RACE_UI_DIR}
        ${RACE_COMMON_INCLUDE_DIR}
    )
    target_compile_options(hud_perf_benchmark PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(hud_perf_benchmark PRIVATE ui_draw ui_font ui_image m)

    add_executable(test_video_compositor_argb
        tests/test_video_compositor_argb.c
        ${RACE_VIDEO_SOURCE_DIR}/video_compositor.c
        ${RACE_VIDEO_SOURCE_DIR}/video_frame.c
    )
    target_include_directories(test_video_compositor_argb PRIVATE
        ${RACE_VIDEO_INCLUDE_DIR}
        ${RACE_UI_DIR}
        ${RACE_HUD_DIR}
    )
    target_compile_definitions(test_video_compositor_argb PRIVATE
        _DEFAULT_SOURCE
        _POSIX_C_SOURCE=200809L
    )
    target_compile_options(test_video_compositor_argb PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_link_libraries(test_video_compositor_argb PRIVATE ${CMAKE_DL_LIBS})
    add_test(NAME video_compositor_rgba_argb_layer_decode
             COMMAND test_video_compositor_argb)

    add_executable(test_shared_state tests/test_shared_state.c)
    target_link_libraries(test_shared_state PRIVATE race_common race_ipc)
    target_compile_options(test_shared_state PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME shared_state_abi_consistency_and_freshness
             COMMAND test_shared_state)

    add_executable(test_shared_state_stress tests/test_shared_state_stress.c)
    target_link_libraries(test_shared_state_stress PRIVATE race_common race_ipc)
    target_compile_options(test_shared_state_stress PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME shared_state_seqlock_high_frequency_no_torn_reads
             COMMAND test_shared_state_stress)
    add_library(video_lifecycle_fault_preload SHARED
        tests/video_lifecycle_fault_preload.c
    )
    target_compile_options(video_lifecycle_fault_preload PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(video_lifecycle_fault_preload PRIVATE
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )

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
        ${RACE_TERMINAL_CORE_SOURCE_DIR}/vehicle_state_clock.c
    )
    target_include_directories(test_vehicle_state_clock PRIVATE ${RACE_CORE_INCLUDE_DIR})
    target_compile_options(test_vehicle_state_clock PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    add_test(NAME vehicle_state_live_replay_clock_and_gps_freshness
             COMMAND test_vehicle_state_clock)

    add_executable(test_encoded_frame_queue
        tests/test_encoded_frame_queue.c
        ${RACE_VIDEO_SOURCE_DIR}/encoded_frame_queue.c
    )
    target_include_directories(test_encoded_frame_queue PRIVATE ${RACE_VIDEO_INCLUDE_DIR})
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
        ${RACE_TERMINAL_CORE_SOURCE_DIR}/can_decoder.c
    )
    target_include_directories(test_can_decoder PRIVATE
        ${RACE_COMMON_LEGACY_INCLUDE_DIR} ${RACE_CORE_INCLUDE_DIR})
    target_compile_options(test_can_decoder PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(test_can_decoder PRIVATE race_common Threads::Threads)
    add_test(NAME can_decoder_freshness COMMAND test_can_decoder)

    add_executable(test_lap_timer_trace
        tests/test_lap_timer_trace.c
        ${RACE_TERMINAL_CORE_SOURCE_DIR}/lap_timer.c
        ${RACE_TERMINAL_CORE_SOURCE_DIR}/gps_track.c
    )
    target_include_directories(test_lap_timer_trace PRIVATE
        ${RACE_COMMON_LEGACY_INCLUDE_DIR} ${RACE_CORE_INCLUDE_DIR})
    target_compile_options(test_lap_timer_trace PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(test_lap_timer_trace PRIVATE
        race_common
        Threads::Threads
        m
    )
    add_test(NAME lap_timer_trace_async COMMAND test_lap_timer_trace)

    add_executable(test_lap_timer_best_reference
        tests/test_lap_timer_best_reference.c
        ${RACE_TERMINAL_CORE_SOURCE_DIR}/lap_timer.c
        ${RACE_TERMINAL_CORE_SOURCE_DIR}/gps_track.c
    )
    target_include_directories(test_lap_timer_best_reference PRIVATE
        ${RACE_COMMON_LEGACY_INCLUDE_DIR} ${RACE_CORE_INCLUDE_DIR})
    target_compile_options(test_lap_timer_best_reference PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(test_lap_timer_best_reference PRIVATE
        race_common
        Threads::Threads
        m
    )
    add_test(NAME lap_timer_best_reference
             COMMAND test_lap_timer_best_reference)

    add_executable(test_race_event_accel
        tests/test_race_event_accel.c
        ${RACE_TERMINAL_CORE_SOURCE_DIR}/race_event.c
        ${RACE_TERMINAL_CORE_SOURCE_DIR}/accel_test.c
        ${RACE_TERMINAL_CORE_SOURCE_DIR}/lap_timer.c
        ${RACE_TERMINAL_CORE_SOURCE_DIR}/gps_track.c
    )
    target_include_directories(test_race_event_accel PRIVATE
        ${RACE_COMMON_LEGACY_INCLUDE_DIR} ${RACE_CORE_INCLUDE_DIR})
    target_compile_options(test_race_event_accel PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(test_race_event_accel PRIVATE
        race_common
        Threads::Threads
        m
    )
    add_test(NAME race_event_parallel_accel COMMAND test_race_event_accel)

    add_executable(test_race_event_laptime
        tests/test_race_event_laptime.c
        ${RACE_TERMINAL_CORE_SOURCE_DIR}/race_event.c
        ${RACE_TERMINAL_CORE_SOURCE_DIR}/accel_test.c
        ${RACE_TERMINAL_CORE_SOURCE_DIR}/lap_timer.c
        ${RACE_TERMINAL_CORE_SOURCE_DIR}/gps_track.c
    )
    target_include_directories(test_race_event_laptime PRIVATE
        ${RACE_COMMON_LEGACY_INCLUDE_DIR} ${RACE_CORE_INCLUDE_DIR})
    target_compile_options(test_race_event_laptime PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(test_race_event_laptime PRIVATE
        race_common
        Threads::Threads
        m
    )
    add_test(NAME race_event_laptime COMMAND test_race_event_laptime)

    add_executable(test_accel_axis
        tests/test_accel_axis.c
        ${RACE_TERMINAL_CORE_SOURCE_DIR}/accel_test.c
    )
    target_include_directories(test_accel_axis PRIVATE
        ${RACE_COMMON_LEGACY_INCLUDE_DIR} ${RACE_CORE_INCLUDE_DIR})
    target_compile_options(test_accel_axis PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(test_accel_axis PRIVATE
        race_common
        Threads::Threads
        m
    )
    add_test(NAME accel_axis_acquisition COMMAND test_accel_axis)

    add_executable(test_replay_vehicle_state_snapshot
        tests/test_replay_vehicle_state_snapshot.c
        ${RACE_TERMINAL_CORE_SOURCE_DIR}/replay.c
        ${RACE_TERMINAL_CORE_SOURCE_DIR}/vehicle_state_snapshot.c
    )
    target_include_directories(test_replay_vehicle_state_snapshot PRIVATE
        ${RACE_COMMON_LEGACY_INCLUDE_DIR} ${RACE_CORE_INCLUDE_DIR}
    )
    target_compile_options(test_replay_vehicle_state_snapshot PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(test_replay_vehicle_state_snapshot PRIVATE
        race_common
        Threads::Threads
        m
    )
    add_test(NAME replay_vehicle_state_snapshot
             COMMAND test_replay_vehicle_state_snapshot)

    add_executable(test_hmi
        tests/test_hmi.c
        ${RACE_TERMINAL_CORE_SOURCE_DIR}/hmi.c
    )
    target_include_directories(test_hmi PRIVATE
        ${RACE_COMMON_LEGACY_INCLUDE_DIR} ${RACE_CORE_INCLUDE_DIR})
    target_compile_options(test_hmi PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(test_hmi PRIVATE
        race_common
        Threads::Threads
        m
    )
    add_test(NAME hmi_ascii_rs485_protocol COMMAND test_hmi)

    add_executable(test_hud_text
        tests/test_hud_text.c
        ${RACE_VIDEO_SOURCE_DIR}/hud_text.c
        ${RACE_VIDEO_SOURCE_DIR}/hud_blend.c
    )
    target_include_directories(test_hud_text PRIVATE
        ${RACE_VIDEO_INCLUDE_DIR}
        ${RACE_UI_DIR}
        ${RACE_HUD_DIR}
    )
    target_compile_options(test_hud_text PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(test_hud_text PRIVATE t113_freetype)
    add_test(NAME hud_text_freetype_cache_metrics_alignment_and_antialias
             COMMAND test_hud_text ${CMAKE_SOURCE_DIR}/assets/fonts/hud.ttf)

    add_executable(test_video_recorder_timestamps
        tests/test_video_recorder_timestamps.c
        ${RACE_VIDEO_SOURCE_DIR}/encoded_frame_queue.c
        ${RACE_VIDEO_SOURCE_DIR}/video_recorder.c
        ${RACE_VIDEO_SOURCE_DIR}/video_encoder.c
        ${RACE_VIDEO_SOURCE_DIR}/jpeg_codec.c
        ${RACE_VIDEO_SOURCE_DIR}/video_frame.c
    )
    target_include_directories(test_video_recorder_timestamps PRIVATE
        ${RACE_COMMON_LEGACY_INCLUDE_DIR}
        ${RACE_VIDEO_INCLUDE_DIR}
        ${T113_JPEG_INCLUDE_DIR}
    )
    target_compile_options(test_video_recorder_timestamps PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(test_video_recorder_timestamps PRIVATE
        race_common
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )
    add_test(NAME video_recorder_variable_timestamps
             COMMAND test_video_recorder_timestamps)

    add_test(NAME video_seconds_above_recorder_limit_rejected
             COMMAND t113_terminal --video-seconds 3601)
    set_tests_properties(video_seconds_above_recorder_limit_rejected
                         PROPERTIES WILL_FAIL TRUE)

    add_executable(test_jpeg_codec
        tests/test_jpeg_codec.c
        ${RACE_VIDEO_SOURCE_DIR}/jpeg_codec.c
        ${RACE_VIDEO_SOURCE_DIR}/video_frame.c
    )
    target_include_directories(test_jpeg_codec PRIVATE
        ${RACE_COMMON_LEGACY_INCLUDE_DIR}
        ${RACE_VIDEO_INCLUDE_DIR}
        ${T113_JPEG_INCLUDE_DIR}
    )
    target_compile_options(test_jpeg_codec PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(test_jpeg_codec PRIVATE race_common ${CMAKE_DL_LIBS})
endif()
