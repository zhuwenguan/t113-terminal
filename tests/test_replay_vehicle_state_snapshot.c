#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "lappro.h"
#include "replay.h"
#include "vehicle_state_snapshot.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    unsigned int can_count;
    unsigned int lappro_count;
    unsigned int snapshot_count;
    uint64_t first_snapshot_us;
    uint64_t last_snapshot_us;
    unsigned int state_get_count;
} TestCounts;

static LapProSampleSink test_lappro_sink;
static void *test_lappro_sink_context;

/* Replay only needs the decoded-state adapter; keep this test independent of
 * Bluetooth and exercise the same callback path as the real adapter. */
void lappro_config_default(LapProConfig *config)
{
    if (config != NULL) {
        memset(config, 0, sizeof(*config));
    }
}

int lappro_init(LapPro *lappro, const LapProConfig *config)
{
    (void)config;
    memset(lappro, 0, sizeof(*lappro));
    lappro->initialized = true;
    return 0;
}

void lappro_set_sample_sink(LapPro *lappro, LapProSampleSink sink,
                            void *context)
{
    if (lappro != NULL) {
        test_lappro_sink = sink;
        test_lappro_sink_context = context;
    }
}

void lappro_update_state(LapPro *lappro, uint64_t timestamp_us,
                         const LapProState *sample)
{
    if (lappro != NULL && test_lappro_sink != NULL) {
        test_lappro_sink(timestamp_us, sample, test_lappro_sink_context);
    }
}

void lappro_stop(LapPro *lappro)
{
    if (lappro != NULL) {
        lappro->initialized = false;
    }
    test_lappro_sink = NULL;
    test_lappro_sink_context = NULL;
}

static void test_can_sink(const CanMessage *message, void *context)
{
    TestCounts *counts = context;

    assert(message != NULL);
    counts->can_count++;
}

static void test_lappro_sample_sink(uint64_t timestamp_us,
                                    const LapProState *state, void *context)
{
    TestCounts *counts = context;

    assert(state != NULL);
    assert(timestamp_us >= 1000000ULL);
    counts->lappro_count++;
}

static void test_snapshot_sink(uint64_t timestamp_us, void *context)
{
    TestCounts *counts = context;

    if (counts->snapshot_count == 0U) {
        counts->first_snapshot_us = timestamp_us;
    } else {
        assert(timestamp_us - counts->last_snapshot_us ==
               VEHICLE_STATE_SNAPSHOT_PERIOD_US);
    }
    counts->last_snapshot_us = timestamp_us;
    counts->snapshot_count++;
}

static bool test_state_getter(VehicleState *state, void *context)
{
    TestCounts *counts = context;

    assert(state != NULL);
    memset(state, 0, sizeof(*state));
    state->speed_kmh = 42.0;
    counts->state_get_count++;
    return true;
}

static void test_write_inputs(const char *directory, unsigned int rows)
{
    char can_path[256];
    char lappro_path[256];
    FILE *can_file;
    FILE *lappro_file;
    unsigned int row;

    assert(snprintf(can_path, sizeof(can_path), "%s/can0.csv", directory) >
           0);
    assert(snprintf(lappro_path, sizeof(lappro_path), "%s/lappro.csv",
                    directory) > 0);
    can_file = fopen(can_path, "w");
    lappro_file = fopen(lappro_path, "w");
    assert(can_file != NULL && lappro_file != NULL);
    fputs("timestamp_us,bus,id,flags,dlc,data\n", can_file);
    fputs("timestamp_us,gps_time,connected,authenticated,streaming,fix_type,"
          "num_sv,speed_kmh,heading_deg,hacc_m,altitude,longitude,latitude,"
          "acc_x,acc_y,acc_z\n",
          lappro_file);
    for (row = 0U; row < rows; ++row) {
        uint64_t timestamp_us = 1000000ULL + (uint64_t)row * 1000ULL;

        assert(fprintf(can_file, "%llu,0,0x121,0,4,00 00 00 00\n",
                       (unsigned long long)timestamp_us) > 0);
        assert(fprintf(lappro_file,
                       "%llu,n/a,1,1,1,3,10,50.0,90.0,1.0,100.0,"
                       "120.0,30.0,0.1,0.2,0.3\n",
                       (unsigned long long)timestamp_us) > 0);
    }
    assert(fclose(can_file) == 0);
    assert(fclose(lappro_file) == 0);
}

int main(void)
{
    char directory_template[] = "/tmp/t113-replay-snapshot-XXXXXX";
    ReplayConfig config;
    TestCounts counts;
    char *directory;

    directory = mkdtemp(directory_template);
    assert(directory != NULL);
    test_write_inputs(directory, 2001U);

    memset(&counts, 0, sizeof(counts));
    memset(&config, 0, sizeof(config));
    config.directory = directory;
    config.speed = 0.0;
    config.can_sink = test_can_sink;
    config.lappro_sink = test_lappro_sample_sink;
    config.vehicle_state_getter = test_state_getter;
    config.vehicle_state_snapshot_sink = test_snapshot_sink;
    config.sink_context = &counts;
    assert(replay_run(&config) == 0);

    /* Two 1 kHz raw inputs must remain lossless at the replay callback path;
     * Vehicle State advances independently at 20 Hz over the same 2-second
     * source-time interval. */
    assert(counts.can_count == 2001U);
    assert(counts.lappro_count == 2001U);
    assert(counts.snapshot_count == 40U);
    assert(counts.first_snapshot_us == 1050000ULL);
    assert(counts.last_snapshot_us == 3000000ULL);
    assert(counts.state_get_count > 0U);

    {
        char can_path[256];
        char lappro_path[256];

        assert(snprintf(can_path, sizeof(can_path), "%s/can0.csv",
                        directory) > 0);
        assert(snprintf(lappro_path, sizeof(lappro_path), "%s/lappro.csv",
                        directory) > 0);
        assert(unlink(can_path) == 0);
        assert(unlink(lappro_path) == 0);
    }
    assert(rmdir(directory) == 0);
    return 0;
}
