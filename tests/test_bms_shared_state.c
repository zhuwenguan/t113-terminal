#include <race/bms_shared_state.h>

#include "test_check.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    char name[64];
    RaceSharedBmsStateHandle writer;
    RaceSharedBmsStateHandle reader;
    BmsState input;
    BmsState output;
    uint64_t published = 0U;
    uint32_t sequence = 0U;

    (void)snprintf(name, sizeof(name), "/race_bms_test_%ld", (long)getpid());
    (void)shm_unlink(name);
    memset(&input, 0, sizeof(input));
    input.valid = true;
    input.board[3].cell_voltage_v[23] = 3.916f;
    input.board[3].cell_valid[23] = true;
    input.hv4_v = 387.45f;
    input.fault_event_count = 1U;
    input.fault_events[0].board = 1U;
    input.fault_events[0].channel = 12U;
    input.fault_events[0].type = BMS_FAULT_CELL_VOLTAGE_OPEN;
    input.fault_events[0].active = true;
    memset(&writer, 0, sizeof(writer));
    writer.fd = -1;
    memset(&reader, 0, sizeof(reader));
    reader.fd = -1;
    TEST_CHECK(race_shared_bms_state_writer_open(&writer, name) == 0);
    {
        pid_t child = fork();
        int status;

        TEST_CHECK(child >= 0);
        if (child == 0) {
            RaceSharedBmsStateHandle contender;
            int result = race_shared_bms_state_writer_open(&contender, name);

            if (result == 0) {
                race_shared_bms_state_close(&contender);
            }
            _exit(result < 0 && errno == EBUSY ? EXIT_SUCCESS : EXIT_FAILURE);
        }
        TEST_CHECK(waitpid(child, &status, 0) == child);
        TEST_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS);
    }
    TEST_CHECK(race_shared_bms_state_reader_open(&reader, name) == 0);
    TEST_CHECK(race_shared_bms_state_publish(&writer, &input, 1000000U) == 0);
    TEST_CHECK(race_shared_bms_state_read(&reader, &output, &published,
                                          &sequence) == 0);
    TEST_CHECK((sequence & 1U) == 0U && published == 1000000U);
    TEST_CHECK(output.valid && output.hv4_v == input.hv4_v);
    TEST_CHECK(output.board[3].cell_voltage_v[23] == 3.916f);
    TEST_CHECK(output.fault_event_count == 1U);
    TEST_CHECK(output.fault_events[0].channel == 12U);
    TEST_CHECK(!race_shared_bms_state_is_stale(1999999U, published,
                                               BMS_STALE_TIMEOUT_US));
    TEST_CHECK(race_shared_bms_state_is_stale(2000001U, published,
                                              BMS_STALE_TIMEOUT_US));
    {
        RaceSharedBmsState incompatible = *reader.mapping;

        incompatible.magic = 0U;
        TEST_CHECK(race_shared_bms_state_validate(&incompatible) < 0 &&
                   errno == EPROTO);
    }
    race_shared_bms_state_close(&reader);
    race_shared_bms_state_close(&writer);
    (void)shm_unlink(name);
    return 0;
}
