#define _POSIX_C_SOURCE 200809L

#include <race/core_status.h>

#include "test_check.h"

#include <stdio.h>
#include <sys/mman.h>

int main(void)
{
    const char *name = "/test_race_core_status";
    RaceCoreStatusHandle writer;
    RaceCoreStatusHandle reader;
    RaceCoreStatusData input;
    RaceCoreStatusData output;

    shm_unlink(name);
    TEST_CHECK(race_core_status_writer_open(&writer, name) == 0);
    TEST_CHECK(race_core_status_reader_open(&reader, name) == 0);
    input.state = RACE_CORE_LIFECYCLE_RUNNING;
    input.flags = RACE_CORE_STATUS_LOGGER_READY |
                  RACE_CORE_STATUS_CAN0_READY |
                  RACE_CORE_STATUS_CAN1_READY;
    input.can_ready_mask = 3U;
    input.heartbeat_mono_us = 123456U;
    (void)snprintf(input.session_directory, sizeof(input.session_directory),
                   "/mnt/usb/sda/log/session_test");
    TEST_CHECK(race_core_status_publish(&writer, &input) == 0);
    TEST_CHECK(race_core_status_read(&reader, &output) == 0);
    TEST_CHECK(output.state == input.state);
    TEST_CHECK(output.flags == input.flags);
    TEST_CHECK(output.can_ready_mask == input.can_ready_mask);
    TEST_CHECK(output.heartbeat_mono_us == input.heartbeat_mono_us);
    TEST_CHECK(output.session_directory[0] == '/');
    race_core_status_close(&reader);
    race_core_status_close(&writer);
    shm_unlink(name);
    return 0;
}
