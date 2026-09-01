#ifndef T113_WEBD_JSON_H
#define T113_WEBD_JSON_H

#include <stddef.h>

#include "state_reader.h"
#include "video_cache.h"

int webd_json_state(char *buffer, size_t buffer_size,
                    const StateSnapshot *snapshot);

int webd_json_state_error(char *buffer, size_t buffer_size,
                          const char *error_message);

int webd_json_system(char *buffer, size_t buffer_size,
                     uint64_t uptime_s, const StateSnapshot *snapshot,
                     const WebdVideoSnapshot *video,
                     unsigned int websocket_clients, unsigned int video_clients,
                     uint64_t now_mono_us);

int webd_json_capabilities(char *buffer, size_t buffer_size);

#endif
