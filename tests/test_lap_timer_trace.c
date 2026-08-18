#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "console_log.h"
#include "gps_track.h"
#include "lap_timer.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static bool test_file_exists(const char *path)
{
    struct stat status;

    return stat(path, &status) == 0 && S_ISREG(status.st_mode);
}

static bool test_wait_for_file(const char *path)
{
    struct timespec delay = {0, 1000000L};
    unsigned int attempt;

    for (attempt = 0U; attempt < 500U; ++attempt) {
        if (test_file_exists(path)) {
            return true;
        }
        nanosleep(&delay, NULL);
    }
    return false;
}

static void test_make_track(GpsTrack *track, GpsTrackPoint *first,
                            GpsTrackPoint *second, GpsTrackPoint *third,
                            GpsTrackPoint *fourth, GpsTrackPoint *finish)
{
    gps_track_init(track);
    assert(gps_track_append(track, 1000000ULL, 0.0, 0.0, 10.0, 0.0, 1.0,
                            first));
    assert(gps_track_append(track, 2000000ULL, 0.0, 0.001, 10.0, 90.0, 1.0,
                            second));
    assert(gps_track_append(track, 3000000ULL, 0.001, 0.001, 10.0, 0.0, 1.0,
                            third));
    assert(gps_track_append(track, 4000000ULL, 0.001, 0.0, 10.0, 270.0, 1.0,
                            fourth));
    assert(gps_track_append(track, 5000000ULL, 0.0, 0.0, 10.0, 0.0, 1.0,
                            finish));
}

static void test_read_header_and_marker(const char *trace_path,
                                        const char *expected_marker,
                                        bool expect_sample)
{
    FILE *file;
    char line[256];

    file = fopen(trace_path, "r");
    assert(file != NULL);
    assert(fgets(line, sizeof(line), file) != NULL);
    assert(strcmp(line,
                  "timestamp_us,distance_s_m,time_s,x_m,y_m,speed_kmh,"
                  "heading_deg\n") == 0);
    if (expect_sample) {
        assert(fgets(line, sizeof(line), file) != NULL);
    } else {
        assert(fgets(line, sizeof(line), file) == NULL);
    }
    fclose(file);

    if (expected_marker != NULL) {
        file = fopen(expected_marker, "r");
        assert(file != NULL);
        assert(fgets(line, sizeof(line), file) != NULL);
        assert(strcmp(line, "old trace\n") == 0);
        fclose(file);
    }
}

int main(void)
{
    char directory_template[] = "/tmp/t113-lap-trace-XXXXXX";
    char old_path[PATH_MAX];
    char first_path[PATH_MAX];
    char second_path[PATH_MAX];
    GpsTrack track;
    GpsTrackPoint first;
    GpsTrackPoint second;
    GpsTrackPoint third;
    GpsTrackPoint fourth;
    GpsTrackPoint finish;
    GpsTrackPoint point;
    VehicleState state;
    LapTimer timer;
    char *directory;
    FILE *old_file;

    console_log_init();
    directory = mkdtemp(directory_template);
    assert(directory != NULL);
    assert(snprintf(old_path, sizeof(old_path), "%s/lap_001.csv",
                    directory) < (int)sizeof(old_path));
    assert(snprintf(first_path, sizeof(first_path), "%s/lap_001_001.csv",
                    directory) < (int)sizeof(first_path));
    assert(snprintf(second_path, sizeof(second_path), "%s/lap_002.csv",
                    directory) < (int)sizeof(second_path));

    old_file = fopen(old_path, "w");
    assert(old_file != NULL);
    assert(fputs("old trace\n", old_file) >= 0);
    assert(fclose(old_file) == 0);

    memset(&state, 0, sizeof(state));
    test_make_track(&track, &first, &second, &third, &fourth, &finish);
    lap_timer_init(&timer, directory);
    assert(timer.trace_thread_started);
    assert(lap_timer_configure_from_loop(&timer, &track, 0U, 4U, 0U,
                                         1000000ULL, &finish, &state));

    point = first;
    point.timestamp_us = 4000000ULL;
    lap_timer_update(&timer, &point, &state);
    point = second;
    point.timestamp_us = 5000000ULL;
    lap_timer_update(&timer, &point, &state);
    point = third;
    point.timestamp_us = 6000000ULL;
    lap_timer_update(&timer, &point, &state);
    point = fourth;
    point.timestamp_us = 7000000ULL;
    lap_timer_update(&timer, &point, &state);
    point = first;
    point.timestamp_us = 8000000ULL;
    lap_timer_update(&timer, &point, &state);

    assert(test_wait_for_file(first_path));
    assert(test_wait_for_file(second_path));
    lap_timer_close(&timer);
    assert(timer.trace_dropped_records == 0U);
    assert(timer.trace_write_errors == 0U);
    test_read_header_and_marker(first_path, old_path, false);
    test_read_header_and_marker(second_path, NULL, true);

    assert(unlink(old_path) == 0);
    assert(unlink(first_path) == 0);
    assert(unlink(second_path) == 0);
    assert(rmdir(directory) == 0);
    return 0;
}
