#ifndef T113_TERMINAL_APP_CONFIG_H
#define T113_TERMINAL_APP_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include <linux/can.h>
#include <net/if.h>

#define APP_DEFAULT_INTERFACE "awlink0"
#define APP_DEFAULT_CAN_ID 0x123U
#define APP_DEFAULT_PERIOD_MS 1000U

enum app_mode {
    MODE_RECEIVE,
    MODE_SEND
};

struct app_config {
    char ifname[IF_NAMESIZE];
    enum app_mode mode;
    canid_t can_id;
    bool id_set;
    bool extended;
    uint8_t data[CAN_MAX_DLEN];
    uint8_t data_len;
    unsigned int period_ms;
    unsigned long count;
};

void app_config_init(struct app_config *config);

/* Returns 0 on success, 1 when --help was requested, and -1 on error. */
int app_config_parse(int argc, char *argv[], struct app_config *config);

void app_config_print_usage(const char *program);

#endif
