#ifndef T113_BMS_PARSER_H
#define T113_BMS_PARSER_H

#include <stdbool.h>
#include <stdint.h>

#include "can_receiver.h"
#include <race/bms_state.h>

/* The existing CAN matrix places the BMS inner network on awlink1. */
#define BMS_PARSER_BUS 1U
#define BMS_PARSER_ID_KPE_UI 0x00eU
#define BMS_PARSER_ID_KPE_AP 0x01eU
#define BMS_PARSER_CELL_VOLTAGE_BASE 0x100U
#define BMS_PARSER_TEMPERATURE_BASE 0x200U
#define BMS_PARSER_FAULT_30_BASE 0x300U
#define BMS_PARSER_FAULT_31_BASE 0x310U

typedef struct {
    BmsState state;
} BmsParser;

void bms_parser_init(BmsParser *parser);

/* Returns true only when a complete, standard BMS frame was accepted. */
bool bms_parser_process(BmsParser *parser, const CanMessage *message);

void bms_parser_expire_stale(BmsParser *parser, uint64_t now_mono_us);

void bms_parser_snapshot(BmsParser *parser, uint64_t now_mono_us,
                         BmsState *state);

#endif
