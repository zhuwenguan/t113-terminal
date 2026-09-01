add_library(race_ipc STATIC
    src/ipc_shm.c
)

target_include_directories(race_ipc PUBLIC
    ${RACE_IPC_INCLUDE_DIR}
    ${RACE_VEHICLE_STATE_INCLUDE_DIR}
)
target_compile_options(race_ipc PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(race_ipc PUBLIC Threads::Threads)
