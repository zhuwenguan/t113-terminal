#include <race/shared_state.h>
#include "test_check.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    char name[64];
    RaceSharedStateHandle writer, reader;
    VehicleState input, output;
    uint64_t published = 0U;
    uint32_t sequence = 0U;
    snprintf(name, sizeof(name), "/race_test_%ld", (long)getpid());
    shm_unlink(name);
    memset(&input, 0, sizeof(input)); input.speed_kmh = 123.5;
    input.motor_rpm = 4567.0; input.lap_number = 3U;
    TEST_CHECK(race_shared_state_writer_open(&writer, name) == 0);
    {
        pid_t child = fork();
        int status;

        TEST_CHECK(child >= 0);
        if (child == 0) {
            RaceSharedStateHandle contender;
            int result = race_shared_state_writer_open(&contender, name);
            if (result == 0) race_shared_state_close(&contender);
            _exit(result < 0 && errno == EBUSY ? EXIT_SUCCESS : EXIT_FAILURE);
        }
        TEST_CHECK(waitpid(child, &status, 0) == child);
        TEST_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS);
    }
    TEST_CHECK(race_shared_state_reader_open(&reader, name) == 0);
    TEST_CHECK(race_shared_state_publish(&writer, &input, 1000000U) == 0);
    TEST_CHECK(race_shared_state_read(&reader, &output, &published, &sequence) == 0);
    TEST_CHECK((sequence & 1U) == 0U && sequence >= 2U &&
           published == 1000000U);
    TEST_CHECK(output.speed_kmh == input.speed_kmh && output.motor_rpm == input.motor_rpm);
    TEST_CHECK(!race_shared_state_is_stale(1999999U, published, 1000000U));
    TEST_CHECK(race_shared_state_is_stale(2000001U, published, 1000000U));
    {
        RaceSharedState incompatible = *reader.mapping;
        incompatible.magic = 0U;
        TEST_CHECK(race_shared_state_validate(&incompatible) < 0 && errno == EPROTO);
    }
    race_shared_state_close(&reader); race_shared_state_close(&writer);
    shm_unlink(name);
    return 0;
}
