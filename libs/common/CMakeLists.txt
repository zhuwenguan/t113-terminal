add_library(race_common STATIC
    src/console_log.c
    src/time_utils.c
)

target_include_directories(race_common PUBLIC ${RACE_COMMON_INCLUDE_DIR})
target_compile_options(race_common PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(race_common PUBLIC Threads::Threads)
