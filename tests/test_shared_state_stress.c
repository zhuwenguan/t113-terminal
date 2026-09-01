#include <race/shared_state.h>

#include "test_check.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void producer(const char *name)
{
    RaceSharedStateHandle writer;
    unsigned int index;

    memset(&writer, 0, sizeof(writer));
    writer.fd = -1;
    if (race_shared_state_writer_open(&writer, name) < 0) {
        _exit(2);
    }
    for (index = 1U; index <= 200000U; ++index) {
        VehicleState state;

        memset(&state, 0, sizeof(state));
        state.timestamp_us = index;
        state.valid_mask = index;
        state.speed_kmh = (double)index;
        state.motor_rpm = (double)index + 0.125;
        state.torque_nm = (double)index + 0.25;
        state.lap_number = index;
        if (race_shared_state_publish(&writer, &state, (uint64_t)index) < 0) {
            race_shared_state_close(&writer);
            _exit(3);
        }
    }
    race_shared_state_close(&writer);
    _exit(0);
}

int main(void)
{
    char name[64];
    RaceSharedStateHandle reader;
    pid_t child;
    int status;
    unsigned int reads = 0U;
    unsigned int valid_reads = 0U;
    unsigned int open_attempts = 0U;

    snprintf(name, sizeof(name), "/race_stress_%ld", (long)getpid());
    (void)shm_unlink(name);
    child = fork();
    TEST_CHECK(child >= 0);
    if (child == 0) {
        producer(name);
    }
    memset(&reader, 0, sizeof(reader));
    reader.fd = -1;
    while (race_shared_state_reader_open(&reader, name) < 0 &&
           open_attempts++ < 3000U) {
        struct timespec delay = {0, 1000000L};
        TEST_CHECK(errno == ENOENT || errno == EPROTO || errno == EAGAIN);
        (void)nanosleep(&delay, NULL);
        if (waitpid(child, &status, WNOHANG) == child) {
            TEST_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
            break;
        }
    }
    TEST_CHECK(reader.mapping != NULL);
    if (reader.mapping != NULL) {
        while (reads < 500000U) {
            VehicleState state;
            uint64_t published = 0U;
            uint32_t sequence = 0U;

            ++reads;
            if (race_shared_state_read(&reader, &state, &published,
                                        &sequence) < 0) {
                TEST_CHECK(errno == EAGAIN);
                continue;
            }
            if (published == 0U) {
                continue;
            }
            ++valid_reads;
            TEST_CHECK((sequence & 1U) == 0U);
            TEST_CHECK(state.timestamp_us == published);
            TEST_CHECK(state.valid_mask == (uint32_t)published);
            TEST_CHECK(state.lap_number == (uint32_t)published);
            TEST_CHECK(state.speed_kmh == (double)published);
            TEST_CHECK(state.motor_rpm == (double)published + 0.125);
            TEST_CHECK(state.torque_nm == (double)published + 0.25);
        }
    }
    if (reader.mapping != NULL) {
        race_shared_state_close(&reader);
    }
    if (waitpid(child, &status, 0) != child) {
        TEST_CHECK(false);
    }
    TEST_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    TEST_CHECK(reads > 1000U);
    TEST_CHECK(valid_reads > 100U);
    (void)shm_unlink(name);
    return 0;
}
