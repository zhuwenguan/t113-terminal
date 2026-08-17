#ifndef T113_LAPPRO_H
#define T113_LAPPRO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pthread.h>

#define LAPPRO_DEFAULT_MAC "B0:A6:04:20:DF:3A"
#define LAPPRO_BDADDR_BREDR 0x00U
#define LAPPRO_BDADDR_LE_PUBLIC 0x01U
#define LAPPRO_BDADDR_LE_RANDOM 0x02U
#define LAPPRO_DEFAULT_ADDRESS_TYPE LAPPRO_BDADDR_LE_PUBLIC

typedef struct {
    char mac[18];
    uint8_t address_type;
} LapProConfig;

typedef struct {
    bool connected;
    bool authenticated;
    bool streaming;
    double speed_kmh;
    double heading_deg;
    double hacc_m;
    double altitude;
    double longitude;
    double latitude;
    double acc_x;
    double acc_y;
    double acc_z;
    uint8_t fix_type;
    uint8_t num_sv;
    uint64_t last_update_monotonic_us;
} LapProState;

typedef void (*LapProSampleSink)(uint64_t timestamp_us,
                                 const LapProState *state, void *context);

typedef struct {
    pthread_mutex_t mutex;
    pthread_t thread;
    LapProConfig config;
    LapProState state;
    LapProSampleSink sample_sink;
    void *sample_sink_context;
    int active_fd;
    bool initialized;
    bool started;
    bool stop_requested;
    uint64_t last_status_report_us;
} LapPro;

void lappro_config_default(LapProConfig *config);

int lappro_init(LapPro *lappro, const LapProConfig *config);

int lappro_start(LapPro *lappro);

void lappro_stop(LapPro *lappro);

void lappro_set_sample_sink(LapPro *lappro, LapProSampleSink sink,
                            void *context);

bool lappro_get_state(const LapPro *lappro, LapProState *state);

#endif
