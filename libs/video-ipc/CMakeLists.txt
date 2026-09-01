add_library(race_video_ipc STATIC
    src/video_ipc.c
)

target_include_directories(race_video_ipc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_options(race_video_ipc PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(race_video_ipc PUBLIC Threads::Threads)
