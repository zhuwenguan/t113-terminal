#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "replay.h"
#include "console_log.h"
#include "vehicle_state_snapshot.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define REPLAY_MAX_LINE_LENGTH 768U
#define REPLAY_MAX_FIELDS 20U
#define REPLAY_STREAM_COUNT 3U

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

typedef enum {
    REPLAY_LOGGER_CAN_ANY = 0,
    REPLAY_LOGGER_CAN0_ONLY,
    REPLAY_LOGGER_CAN1_ONLY,
    REPLAY_LOGGER_CAN_NONE
} ReplayLoggerCanMode;

typedef struct {
    ReplaySource source;
    unsigned int default_bus;
    FILE *file;
    char path[PATH_MAX];
    uint64_t line_number;
    uint64_t last_timestamp_us;
    bool has_timestamp;
    bool has_event;
    ReplayEvent event;
} ReplayStream;

typedef struct {
    char session[NAME_MAX + 1U];
    bool has_can;
    char can_name[NAME_MAX + 1U];
    bool has_can0;
    char can0_name[NAME_MAX + 1U];
    bool has_can1;
    char can1_name[NAME_MAX + 1U];
    bool has_lappro;
    char lappro_name[NAME_MAX + 1U];
} ReplaySessionCandidate;

typedef struct {
    ReplaySessionCandidate *sessions;
    size_t session_count;
    size_t session_capacity;
    bool exact_can0;
    bool exact_can1;
    bool exact_can;
    bool exact_lappro;
} ReplayDiscovery;

typedef struct {
    bool can0;
    char can0_name[NAME_MAX + 1U];
    bool can1;
    char can1_name[NAME_MAX + 1U];
    bool lappro;
    char lappro_name[NAME_MAX + 1U];
} ReplaySelection;

static volatile sig_atomic_t replay_running;

static void replay_signal_handler(int signal_number)
{
    (void)signal_number;
    replay_running = 0;
}

static uint64_t replay_monotonic_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return 0U;
    }

    return (uint64_t)now.tv_sec * 1000000ULL +
           (uint64_t)now.tv_nsec / 1000ULL;
}

static char *replay_trim(char *text)
{
    char *end;

    while (*text != '\0' && isspace((unsigned char)*text) != 0) {
        text++;
    }
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]) != 0) {
        end--;
    }
    *end = '\0';
    return text;
}

static size_t replay_split_csv(char *line, char *fields[], size_t capacity)
{
    size_t count = 0U;
    char *cursor = line;

    while (count < capacity) {
        char *separator = strchr(cursor, ',');

        if (separator != NULL) {
            *separator = '\0';
        }
        fields[count++] = replay_trim(cursor);
        if (separator == NULL) {
            return count;
        }
        cursor = separator + 1;
    }

    return strchr(cursor, ',') == NULL ? count : 0U;
}

static bool replay_parse_u64(const char *text, int base, uint64_t *value)
{
    char *end;
    unsigned long long parsed;

    if (text == NULL || value == NULL || *text == '\0' || *text == '-') {
        return false;
    }
    errno = 0;
    parsed = strtoull(text, &end, base);
    while (isspace((unsigned char)*end) != 0) {
        end++;
    }
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static bool replay_parse_u32(const char *text, int base, uint32_t *value)
{
    uint64_t parsed;

    if (!replay_parse_u64(text, base, &parsed) || parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool replay_parse_double(const char *text, double *value)
{
    char *end;
    double parsed;

    if (text == NULL || value == NULL || *text == '\0') {
        return false;
    }
    errno = 0;
    parsed = strtod(text, &end);
    while (isspace((unsigned char)*end) != 0) {
        end++;
    }
    if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed)) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool replay_parse_bool(const char *text, bool *value)
{
    uint32_t parsed;

    if (!replay_parse_u32(text, 10, &parsed) || parsed > 1U) {
        return false;
    }
    *value = parsed != 0U;
    return true;
}

static bool replay_parse_u8(const char *text, uint8_t *value)
{
    uint32_t parsed;

    if (!replay_parse_u32(text, 10, &parsed) || parsed > UINT8_MAX) {
        return false;
    }
    *value = (uint8_t)parsed;
    return true;
}

static bool replay_parse_data(const char *text, uint8_t *data, uint8_t length)
{
    unsigned int nibble = 0U;
    bool have_nibble = false;
    size_t output = 0U;
    const unsigned char *cursor = (const unsigned char *)text;

    if (text == NULL || data == NULL || length > CAN_MAX_DLEN) {
        return false;
    }
    while (*cursor != '\0') {
        unsigned int value;

        if (isspace(*cursor) != 0) {
            cursor++;
            continue;
        }
        if (*cursor >= '0' && *cursor <= '9') {
            value = *cursor - '0';
        } else if (*cursor >= 'a' && *cursor <= 'f') {
            value = *cursor - 'a' + 10U;
        } else if (*cursor >= 'A' && *cursor <= 'F') {
            value = *cursor - 'A' + 10U;
        } else {
            return false;
        }
        if (!have_nibble) {
            nibble = value;
            have_nibble = true;
        } else {
            if (output >= length) {
                return false;
            }
            data[output++] = (uint8_t)((nibble << 4U) | value);
            have_nibble = false;
        }
        cursor++;
    }

    return !have_nibble && output == length;
}

static bool replay_parse_gps_time(const char *text, LapProState *state)
{
    unsigned int year;
    unsigned int month;
    unsigned int day;
    unsigned int hour;
    unsigned int minute;
    unsigned int second;
    unsigned int millisecond;

    if (text == NULL || state == NULL) {
        return false;
    }
    if (strcmp(text, "n/a") == 0 || strcmp(text, "") == 0) {
        state->gps_time_valid = false;
        return true;
    }
    if (strlen(text) != 23U ||
        sscanf(text, "%u-%u-%uT%u:%u:%u.%u", &year, &month, &day,
               &hour, &minute, &second, &millisecond) != 7) {
        return false;
    }
    if (month == 0U || month > 12U || day == 0U || day > 31U ||
        hour > 23U || minute > 59U || second > 59U || millisecond > 999U) {
        return false;
    }
    state->gps_time_valid = true;
    state->gps_year = (uint16_t)year;
    state->gps_month = (uint8_t)month;
    state->gps_day = (uint8_t)day;
    state->gps_hour = (uint8_t)hour;
    state->gps_minute = (uint8_t)minute;
    state->gps_second = (uint8_t)second;
    state->gps_millisecond = (uint16_t)millisecond;
    return true;
}

static bool replay_parse_can(char *fields[], size_t field_count,
                             unsigned int default_bus, ReplayEvent *event)
{
    uint64_t timestamp_us;
    uint32_t bus;
    uint32_t id;
    uint32_t flags;
    uint32_t dlc;
    const char *data_text;

    if (fields == NULL || event == NULL ||
        (field_count != 4U && field_count != 5U && field_count != 6U)) {
        return false;
    }
    if (!replay_parse_u64(fields[0], 10, &timestamp_us)) {
        return false;
    }

    if (field_count == 4U) {
        bus = default_bus;
        flags = CAN_MESSAGE_FLAG_STANDARD;
        if (!replay_parse_u32(fields[1], 0, &id) ||
            !replay_parse_u32(fields[2], 10, &dlc)) {
            return false;
        }
        data_text = fields[3];
    } else if (field_count == 5U) {
        if (!replay_parse_u32(fields[1], 10, &bus) ||
            !replay_parse_u32(fields[2], 0, &id) ||
            !replay_parse_u32(fields[3], 10, &dlc)) {
            return false;
        }
        flags = CAN_MESSAGE_FLAG_STANDARD;
        data_text = fields[4];
    } else {
        if (!replay_parse_u32(fields[1], 10, &bus) ||
            !replay_parse_u32(fields[2], 0, &id) ||
            !replay_parse_u32(fields[3], 0, &flags) ||
            !replay_parse_u32(fields[4], 10, &dlc)) {
            return false;
        }
        data_text = fields[5];
    }
    if (bus >= CAN_RECEIVER_BUS_COUNT || id > CAN_EFF_MASK ||
        dlc > CAN_MAX_DLEN) {
        return false;
    }

    memset(event, 0, sizeof(*event));
    event->timestamp_us = timestamp_us;
    event->source = bus == 0U ? REPLAY_CAN0 : REPLAY_CAN1;
    event->payload.can.timestamp_us = timestamp_us;
    event->payload.can.bus = (uint8_t)bus;
    event->payload.can.id = id;
    event->payload.can.flags = flags;
    event->payload.can.dlc = (uint8_t)dlc;
    return replay_parse_data(data_text, event->payload.can.data,
                             event->payload.can.dlc);
}

static bool replay_parse_lappro(char *fields[], size_t field_count,
                                ReplayEvent *event)
{
    LapProState *state;
    uint64_t timestamp_us;

    if (fields == NULL || event == NULL ||
        (field_count != 13U && field_count != 16U) ||
        !replay_parse_u64(fields[0], 10, &timestamp_us)) {
        return false;
    }

    memset(event, 0, sizeof(*event));
    event->timestamp_us = timestamp_us;
    event->source = REPLAY_LAPPRO;
    state = &event->payload.lappro;
    state->last_update_monotonic_us = timestamp_us;
    if (!replay_parse_gps_time(fields[1], state)) {
        return false;
    }

    if (field_count == 13U) {
        state->connected = true;
        state->authenticated = true;
        state->streaming = true;
        if (!replay_parse_double(fields[2], &state->latitude) ||
            !replay_parse_double(fields[3], &state->longitude) ||
            !replay_parse_double(fields[4], &state->altitude) ||
            !replay_parse_double(fields[5], &state->speed_kmh) ||
            !replay_parse_double(fields[6], &state->heading_deg) ||
            !replay_parse_double(fields[7], &state->hacc_m) ||
            !replay_parse_u8(fields[8], &state->fix_type) ||
            !replay_parse_u8(fields[9], &state->num_sv) ||
            !replay_parse_double(fields[10], &state->acc_x) ||
            !replay_parse_double(fields[11], &state->acc_y) ||
            !replay_parse_double(fields[12], &state->acc_z)) {
            return false;
        }
    } else {
        if (!replay_parse_bool(fields[2], &state->connected) ||
            !replay_parse_bool(fields[3], &state->authenticated) ||
            !replay_parse_bool(fields[4], &state->streaming) ||
            !replay_parse_u8(fields[5], &state->fix_type) ||
            !replay_parse_u8(fields[6], &state->num_sv)) {
            return false;
        }
        if (!replay_parse_double(fields[7], &state->speed_kmh) ||
            !replay_parse_double(fields[8], &state->heading_deg) ||
            !replay_parse_double(fields[9], &state->hacc_m) ||
            !replay_parse_double(fields[10], &state->altitude) ||
            !replay_parse_double(fields[11], &state->longitude) ||
            !replay_parse_double(fields[12], &state->latitude) ||
            !replay_parse_double(fields[13], &state->acc_x) ||
            !replay_parse_double(fields[14], &state->acc_y) ||
            !replay_parse_double(fields[15], &state->acc_z)) {
            return false;
        }
    }
    return true;
}

static void replay_invalid_line(const ReplayStream *stream)
{
    LOG_WARN("[Replay] invalid line: %s:%llu", stream->path,
             (unsigned long long)stream->line_number);
}

static bool replay_stream_read_next(ReplayStream *stream)
{
    char line[REPLAY_MAX_LINE_LENGTH];

    if (stream == NULL || stream->file == NULL) {
        return false;
    }
    while (fgets(line, sizeof(line), stream->file) != NULL) {
        char *fields[REPLAY_MAX_FIELDS];
        size_t field_count;
        ReplayEvent event;

        stream->line_number++;
        if (strchr(line, '\n') == NULL && !feof(stream->file)) {
            int character;

            do {
                character = fgetc(stream->file);
            } while (character != '\n' && character != EOF);
            replay_invalid_line(stream);
            continue;
        }
        replay_trim(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        field_count = replay_split_csv(line, fields, REPLAY_MAX_FIELDS);
        if (field_count == 0U ||
            (stream->source == REPLAY_LAPPRO
                 ? !replay_parse_lappro(fields, field_count, &event)
                 : !replay_parse_can(fields, field_count, stream->default_bus,
                                     &event))) {
            replay_invalid_line(stream);
            continue;
        }
        if (stream->has_timestamp &&
            event.timestamp_us < stream->last_timestamp_us) {
            replay_invalid_line(stream);
            continue;
        }
        stream->last_timestamp_us = event.timestamp_us;
        stream->has_timestamp = true;
        stream->event = event;
        stream->has_event = true;
        return true;
    }
    stream->has_event = false;
    return false;
}

static bool replay_make_path(const char *directory, const char *name,
                             char *path, size_t path_length)
{
    int written;

    if (directory == NULL || name == NULL || path == NULL) {
        return false;
    }
    written = snprintf(path, path_length, "%s/%s", directory, name);
    return written >= 0 && (size_t)written < path_length;
}

static bool replay_path_is_regular(const char *directory, const char *name)
{
    char path[PATH_MAX];
    struct stat status;

    if (!replay_make_path(directory, name, path, sizeof(path)) ||
        stat(path, &status) < 0) {
        return false;
    }
    return S_ISREG(status.st_mode);
}

static bool replay_copy_filename(char *destination, size_t destination_size,
                                 const char *name)
{
    int written;

    if (destination == NULL || destination_size == 0U || name == NULL) {
        return false;
    }
    written = snprintf(destination, destination_size, "%s", name);
    return written >= 0 && (size_t)written < destination_size;
}

static bool replay_parse_session_filename(const char *name, const char *prefix,
                                          char *session,
                                          size_t session_size)
{
    size_t name_length;
    size_t prefix_length;
    size_t session_length;

    if (name == NULL || prefix == NULL || session == NULL ||
        session_size == 0U) {
        return false;
    }
    name_length = strlen(name);
    prefix_length = strlen(prefix);
    if (name_length <= prefix_length + 4U ||
        strncmp(name, prefix, prefix_length) != 0 ||
        strcmp(name + name_length - 4U, ".csv") != 0) {
        return false;
    }
    session_length = name_length - prefix_length - 4U;
    if (session_length == 0U || session_length >= session_size) {
        return false;
    }
    memcpy(session, name + prefix_length, session_length);
    session[session_length] = '\0';
    return true;
}

static ReplaySessionCandidate *replay_find_session(ReplayDiscovery *discovery,
                                                   const char *session)
{
    size_t index;

    if (discovery == NULL || session == NULL) {
        return NULL;
    }
    for (index = 0U; index < discovery->session_count; ++index) {
        if (strcmp(discovery->sessions[index].session, session) == 0) {
            return &discovery->sessions[index];
        }
    }
    return NULL;
}

static ReplaySessionCandidate *replay_add_session(ReplayDiscovery *discovery,
                                                  const char *session)
{
    ReplaySessionCandidate *candidate;
    size_t new_capacity;

    candidate = replay_find_session(discovery, session);
    if (candidate != NULL) {
        return candidate;
    }
    if (discovery->session_count == discovery->session_capacity) {
        new_capacity = discovery->session_capacity == 0U
                           ? 8U
                           : discovery->session_capacity * 2U;
        candidate = realloc(discovery->sessions,
                            new_capacity * sizeof(*discovery->sessions));
        if (candidate == NULL) {
            return NULL;
        }
        discovery->sessions = candidate;
        discovery->session_capacity = new_capacity;
    }
    candidate = &discovery->sessions[discovery->session_count++];
    memset(candidate, 0, sizeof(*candidate));
    if (!replay_copy_filename(candidate->session,
                              sizeof(candidate->session), session)) {
        discovery->session_count--;
        return NULL;
    }
    return candidate;
}

static bool replay_record_session_file(ReplayDiscovery *discovery,
                                        const char *directory,
                                        const char *name, const char *prefix,
                                        int file_kind)
{
    char session[NAME_MAX + 1U];
    ReplaySessionCandidate *candidate;

    if (!replay_parse_session_filename(name, prefix, session,
                                       sizeof(session))) {
        return false;
    }
    if (!replay_path_is_regular(directory, name)) {
        return true;
    }
    candidate = replay_add_session(discovery, session);
    if (candidate == NULL) {
        LOG_ERROR("[Replay] cannot remember logger session '%s'", session);
        return false;
    }
    switch (file_kind) {
    case 0:
        candidate->has_can = true;
        return replay_copy_filename(candidate->can_name,
                                    sizeof(candidate->can_name), name);
    case 1:
        candidate->has_can0 = true;
        return replay_copy_filename(candidate->can0_name,
                                    sizeof(candidate->can0_name), name);
    case 2:
        candidate->has_can1 = true;
        return replay_copy_filename(candidate->can1_name,
                                    sizeof(candidate->can1_name), name);
    case 3:
        candidate->has_lappro = true;
        return replay_copy_filename(candidate->lappro_name,
                                    sizeof(candidate->lappro_name), name);
    default:
        return false;
    }
}

static void replay_discovery_destroy(ReplayDiscovery *discovery)
{
    if (discovery == NULL) {
        return;
    }
    free(discovery->sessions);
    memset(discovery, 0, sizeof(*discovery));
}

static int replay_discover_inputs(const char *directory,
                                  ReplayDiscovery *discovery)
{
    DIR *directory_handle;
    struct dirent *entry;

    if (directory == NULL || discovery == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(discovery, 0, sizeof(*discovery));
    directory_handle = opendir(directory);
    if (directory_handle == NULL) {
        LOG_ERROR("[Replay] cannot open input directory %s: %s", directory,
                  strerror(errno));
        return -1;
    }
    while ((entry = readdir(directory_handle)) != NULL) {
        const char *name = entry->d_name;

        if (strcmp(name, "can0.csv") == 0) {
            discovery->exact_can0 =
                replay_path_is_regular(directory, name);
            continue;
        }
        if (strcmp(name, "can1.csv") == 0) {
            discovery->exact_can1 =
                replay_path_is_regular(directory, name);
            continue;
        }
        if (strcmp(name, "can.csv") == 0) {
            discovery->exact_can = replay_path_is_regular(directory, name);
            continue;
        }
        if (strcmp(name, "lappro.csv") == 0) {
            discovery->exact_lappro =
                replay_path_is_regular(directory, name);
            continue;
        }
        if (replay_record_session_file(discovery, directory, name, "can_",
                                       0) ||
            replay_record_session_file(discovery, directory, name, "can0_",
                                       1) ||
            replay_record_session_file(discovery, directory, name, "can1_",
                                       2) ||
            replay_record_session_file(discovery, directory, name, "lappro_",
                                       3)) {
            continue;
        }
    }
    closedir(directory_handle);
    return 0;
}

static bool replay_session_has_can(const ReplaySessionCandidate *candidate,
                                   ReplayLoggerCanMode mode)
{
    if (candidate == NULL) {
        return false;
    }
    switch (mode) {
    case REPLAY_LOGGER_CAN_ANY:
        return candidate->has_can || candidate->has_can0 ||
               candidate->has_can1;
    case REPLAY_LOGGER_CAN0_ONLY:
        return candidate->has_can0;
    case REPLAY_LOGGER_CAN1_ONLY:
        return candidate->has_can1;
    case REPLAY_LOGGER_CAN_NONE:
    default:
        return false;
    }
}

static bool replay_has_logger_input(const ReplayDiscovery *discovery,
                                    bool want_can, bool want_lappro,
                                    ReplayLoggerCanMode can_mode)
{
    size_t index;

    if (discovery == NULL) {
        return false;
    }
    for (index = 0U; index < discovery->session_count; ++index) {
        const ReplaySessionCandidate *candidate = &discovery->sessions[index];

        if ((!want_can || replay_session_has_can(candidate, can_mode)) &&
            (!want_lappro || candidate->has_lappro)) {
            return true;
        }
    }
    return false;
}

static const ReplaySessionCandidate *replay_latest_session(
    const ReplayDiscovery *discovery, bool want_can, bool want_lappro,
    ReplayLoggerCanMode can_mode)
{
    const ReplaySessionCandidate *selected = NULL;
    size_t index;

    if (discovery == NULL) {
        return NULL;
    }
    for (index = 0U; index < discovery->session_count; ++index) {
        const ReplaySessionCandidate *candidate = &discovery->sessions[index];

        if ((want_can && !replay_session_has_can(candidate, can_mode)) ||
            (want_lappro && !candidate->has_lappro)) {
            continue;
        }
        if (selected == NULL ||
            strcmp(candidate->session, selected->session) > 0) {
            selected = candidate;
        }
    }
    return selected;
}

static bool replay_selection_set_name(bool *present, char *destination,
                                      size_t destination_size,
                                      const char *name)
{
    if (present == NULL || destination == NULL || name == NULL ||
        !replay_copy_filename(destination, destination_size, name)) {
        return false;
    }
    *present = true;
    return true;
}

/*
 * Canonical names are explicit inputs and take precedence for their stream.
 * Every logger fallback stream is selected from one session candidate. When
 * both logger families are needed, only a shared session is accepted; the
 * lexicographically greatest shared suffix is the deterministic choice.
 */
static bool replay_build_selection(const ReplayDiscovery *discovery,
                                   ReplaySelection *selection)
{
    bool need_can0;
    bool need_can1;
    bool need_lappro;
    bool logger_can_available;
    bool logger_lappro_available;
    ReplayLoggerCanMode can_mode;
    const ReplaySessionCandidate *logger_session = NULL;

    if (discovery == NULL || selection == NULL) {
        return false;
    }
    memset(selection, 0, sizeof(*selection));

    if (discovery->exact_can0) {
        if (!replay_selection_set_name(&selection->can0,
                                       selection->can0_name,
                                       sizeof(selection->can0_name),
                                       "can0.csv")) {
            return false;
        }
    } else if (discovery->exact_can) {
        /* Keep the historical single-file can.csv input as an explicit file. */
        if (!replay_selection_set_name(&selection->can0,
                                       selection->can0_name,
                                       sizeof(selection->can0_name),
                                       "can.csv")) {
            return false;
        }
    }
    if (discovery->exact_can1 &&
        !replay_selection_set_name(&selection->can1, selection->can1_name,
                                   sizeof(selection->can1_name), "can1.csv")) {
        return false;
    }
    if (discovery->exact_lappro &&
        !replay_selection_set_name(&selection->lappro,
                                   selection->lappro_name,
                                   sizeof(selection->lappro_name),
                                   "lappro.csv")) {
        return false;
    }

    need_can0 = !selection->can0;
    need_can1 = !selection->can1;
    need_lappro = !selection->lappro;
    if (need_can0 && need_can1) {
        can_mode = REPLAY_LOGGER_CAN_ANY;
    } else if (need_can0) {
        can_mode = REPLAY_LOGGER_CAN0_ONLY;
    } else if (need_can1) {
        can_mode = REPLAY_LOGGER_CAN1_ONLY;
    } else {
        can_mode = REPLAY_LOGGER_CAN_NONE;
    }

    logger_can_available =
        (need_can0 || need_can1) &&
        replay_has_logger_input(discovery, true, false, can_mode);
    logger_lappro_available =
        need_lappro && replay_has_logger_input(discovery, false, true,
                                               REPLAY_LOGGER_CAN_NONE);

    if (logger_can_available && logger_lappro_available) {
        logger_session = replay_latest_session(discovery, true, true,
                                               can_mode);
        if (logger_session == NULL) {
            LOG_ERROR("[Replay] logger CAN/LapPro files have no common "
                      "session; refusing to mix them");
            return false;
        }
        LOG_INFO("[Replay] selected logger session: %s",
                 logger_session->session);
    } else if (logger_can_available) {
        logger_session = replay_latest_session(discovery, true, false,
                                               can_mode);
        if (logger_session != NULL) {
            LOG_INFO("[Replay] selected logger CAN session: %s",
                     logger_session->session);
        }
    } else if (logger_lappro_available) {
        logger_session = replay_latest_session(discovery, false, true,
                                               REPLAY_LOGGER_CAN_NONE);
        if (logger_session != NULL) {
            LOG_INFO("[Replay] selected logger LapPro session: %s",
                     logger_session->session);
        }
    }

    if (logger_session != NULL && (need_can0 || need_can1)) {
        if (can_mode == REPLAY_LOGGER_CAN_ANY) {
            if (logger_session->has_can) {
                if (!replay_selection_set_name(
                        &selection->can0, selection->can0_name,
                        sizeof(selection->can0_name),
                        logger_session->can_name)) {
                    return false;
                }
            } else {
                if (need_can0 && logger_session->has_can0 &&
                    !replay_selection_set_name(
                        &selection->can0, selection->can0_name,
                        sizeof(selection->can0_name),
                        logger_session->can0_name)) {
                    return false;
                }
                if (need_can1 && logger_session->has_can1 &&
                    !replay_selection_set_name(
                        &selection->can1, selection->can1_name,
                        sizeof(selection->can1_name),
                        logger_session->can1_name)) {
                    return false;
                }
            }
        } else if (can_mode == REPLAY_LOGGER_CAN0_ONLY &&
                   logger_session->has_can0 &&
                   !replay_selection_set_name(
                       &selection->can0, selection->can0_name,
                       sizeof(selection->can0_name),
                       logger_session->can0_name)) {
            return false;
        } else if (can_mode == REPLAY_LOGGER_CAN1_ONLY &&
                   logger_session->has_can1 &&
                   !replay_selection_set_name(
                       &selection->can1, selection->can1_name,
                       sizeof(selection->can1_name),
                       logger_session->can1_name)) {
            return false;
        }
    }
    if (logger_session != NULL && need_lappro &&
        logger_session->has_lappro &&
        !replay_selection_set_name(&selection->lappro,
                                   selection->lappro_name,
                                   sizeof(selection->lappro_name),
                                   logger_session->lappro_name)) {
        return false;
    }

    return selection->can0 || selection->can1 || selection->lappro;
}

static bool replay_open_path(ReplayStream *stream, const char *directory,
                             const char *name)
{
    char path[PATH_MAX];

    if (stream == NULL || directory == NULL || name == NULL ||
        !replay_make_path(directory, name, path, sizeof(path))) {
        return false;
    }
    stream->file = fopen(path, "r");
    if (stream->file == NULL) {
        LOG_ERROR("[Replay] cannot open selected input %s: %s", path,
                  strerror(errno));
        return false;
    }
    snprintf(stream->path, sizeof(stream->path), "%s", path);
    LOG_INFO("[Replay] loading: %s", path);
    return true;
}

static bool replay_open_selected_stream(ReplayStream *stream,
                                         const char *directory,
                                         ReplaySource source,
                                         unsigned int default_bus,
                                         const char *name)
{
    memset(stream, 0, sizeof(*stream));
    stream->source = source;
    stream->default_bus = default_bus;
    return replay_open_path(stream, directory, name);
}

static void replay_close_stream(ReplayStream *stream)
{
    if (stream != NULL && stream->file != NULL) {
        fclose(stream->file);
        stream->file = NULL;
    }
}

static const char *replay_source_name(ReplaySource source)
{
    switch (source) {
    case REPLAY_CAN0:
        return "CAN0";
    case REPLAY_CAN1:
        return "CAN1";
    case REPLAY_LAPPRO:
        return "LapPro";
    default:
        return "unknown";
    }
}

static void replay_add_us(struct timespec *timestamp, uint64_t offset_us)
{
    timestamp->tv_sec += (time_t)(offset_us / 1000000ULL);
    timestamp->tv_nsec += (long)(offset_us % 1000000ULL) * 1000L;
    if (timestamp->tv_nsec >= 1000000000L) {
        timestamp->tv_sec++;
        timestamp->tv_nsec -= 1000000000L;
    }
}

static bool replay_wait_until(const struct timespec *deadline)
{
    while (replay_running) {
        int result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline,
                                     NULL);

        if (result == 0) {
            return true;
        }
        if (result != EINTR) {
            LOG_WARN("[Replay] clock_nanosleep failed: %s", strerror(result));
            return false;
        }
    }
    return false;
}

static void replay_emit_vehicle_state_snapshots(
    const ReplayConfig *config, VehicleStateSnapshotSchedule *schedule,
    uint64_t timestamp_us, bool include_current)
{
    uint64_t snapshot_timestamp_us;

    if (config == NULL || schedule == NULL ||
        config->vehicle_state_snapshot_sink == NULL) {
        return;
    }
    while (vehicle_state_snapshot_schedule_next_due(
        schedule, timestamp_us, include_current, &snapshot_timestamp_us)) {
        config->vehicle_state_snapshot_sink(snapshot_timestamp_us,
                                            config->sink_context);
    }
}

int replay_run(const ReplayConfig *config)
{
    ReplayDiscovery discovery;
    ReplaySelection selection;
    ReplayStream streams[REPLAY_STREAM_COUNT];
    LapPro lappro;
    LapProConfig lappro_config;
    bool lappro_ready = false;
    bool input_open = false;
    bool have_first_timestamp = false;
    bool have_last_event_timestamp = false;
    uint64_t first_timestamp_us = 0U;
    uint64_t last_event_timestamp_us = 0U;
    uint64_t wall_start_us = 0U;
    uint64_t last_report_us = 0U;
    VehicleStateSnapshotSchedule snapshot_schedule;
    struct timespec wall_start;
    struct sigaction signal_action;
    unsigned int stream_index;
    int result = 0;

    if (config == NULL || config->directory == NULL ||
        config->directory[0] == '\0' || config->speed < 0.0 ||
        !isfinite(config->speed) || config->can_sink == NULL ||
        config->lappro_sink == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(&discovery, 0, sizeof(discovery));
    memset(&selection, 0, sizeof(selection));
    memset(streams, 0, sizeof(streams));
    memset(&snapshot_schedule, 0, sizeof(snapshot_schedule));
    LOG_INFO("[Replay] mode enabled");

    if (replay_discover_inputs(config->directory, &discovery) < 0 ||
        !replay_build_selection(&discovery, &selection)) {
        LOG_ERROR("[Replay] no supported input CSV in %s", config->directory);
        result = -1;
        goto cleanup;
    }
    if (selection.can0) {
        if (!replay_open_selected_stream(
                &streams[0], config->directory, REPLAY_CAN0, 0U,
                selection.can0_name)) {
            result = -1;
            goto cleanup;
        }
        input_open = true;
    }
    if (selection.can1) {
        if (!replay_open_selected_stream(
                &streams[1], config->directory, REPLAY_CAN1, 1U,
                selection.can1_name)) {
            result = -1;
            goto cleanup;
        }
        input_open = true;
    }
    if (selection.lappro) {
        if (!replay_open_selected_stream(
                &streams[2], config->directory, REPLAY_LAPPRO, 0U,
                selection.lappro_name)) {
            result = -1;
            goto cleanup;
        }
        input_open = true;
    }
    if (!input_open) {
        LOG_ERROR("[Replay] no supported input CSV in %s", config->directory);
        result = -1;
        goto cleanup;
    }

    memset(&signal_action, 0, sizeof(signal_action));
    signal_action.sa_handler = replay_signal_handler;
    sigemptyset(&signal_action.sa_mask);
    if (sigaction(SIGINT, &signal_action, NULL) < 0 ||
        sigaction(SIGTERM, &signal_action, NULL) < 0) {
        LOG_ERROR("[Replay] cannot install signal handlers: %s",
                  strerror(errno));
        result = -1;
        goto cleanup;
    }
    replay_running = 1;

    lappro_config_default(&lappro_config);
    if (lappro_init(&lappro, &lappro_config) == 0) {
        lappro_ready = true;
        lappro_set_sample_sink(&lappro, config->lappro_sink,
                               config->sink_context);
    } else {
        LOG_WARN("[Replay] LapPro state adapter unavailable; using sink");
    }

    for (stream_index = 0U; stream_index < REPLAY_STREAM_COUNT;
         ++stream_index) {
        if (streams[stream_index].file != NULL) {
            (void)replay_stream_read_next(&streams[stream_index]);
        }
    }

    while (replay_running) {
        ReplayStream *selected = NULL;
        ReplayEvent event;

        for (stream_index = 0U; stream_index < REPLAY_STREAM_COUNT;
             ++stream_index) {
            ReplayStream *stream = &streams[stream_index];

            if (stream->file == NULL || !stream->has_event) {
                continue;
            }
            if (selected == NULL ||
                stream->event.timestamp_us < selected->event.timestamp_us) {
                selected = stream;
            }
        }
        if (selected == NULL) {
            break;
        }
        event = selected->event;

        if (!have_first_timestamp) {
            struct timespec now;

            if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
                LOG_ERROR("[Replay] clock_gettime failed: %s", strerror(errno));
                result = -1;
                break;
            }
            first_timestamp_us = event.timestamp_us;
            vehicle_state_snapshot_schedule_init(&snapshot_schedule,
                                                 first_timestamp_us);
            wall_start = now;
            wall_start_us = replay_monotonic_us();
            have_first_timestamp = true;
        } else if (config->speed > 0.0) {
            uint64_t source_delta_us = event.timestamp_us -
                                       first_timestamp_us;
            uint64_t replay_delta_us =
                (uint64_t)((double)source_delta_us / config->speed);
            struct timespec deadline = wall_start;

            replay_add_us(&deadline, replay_delta_us);
            if (!replay_wait_until(&deadline)) {
                break;
            }
        }

        /* Advance snapshots on replay time, not by a second wall-clock
         * thread. Wait until all events at the previous timestamp have been
         * applied before emitting a tick at that timestamp. */
        if (have_last_event_timestamp &&
            event.timestamp_us > last_event_timestamp_us) {
            replay_emit_vehicle_state_snapshots(config, &snapshot_schedule,
                                                event.timestamp_us, false);
        }

        if (event.source == REPLAY_LAPPRO) {
            if (lappro_ready) {
                lappro_update_state(&lappro, event.timestamp_us,
                                    &event.payload.lappro);
            } else {
                config->lappro_sink(event.timestamp_us, &event.payload.lappro,
                                    config->sink_context);
            }
        } else {
            config->can_sink(&event.payload.can, config->sink_context);
        }

        last_event_timestamp_us = event.timestamp_us;
        have_last_event_timestamp = true;

        {
            uint64_t now_us = replay_monotonic_us();

            if (last_report_us == 0U || now_us - last_report_us >= 1000000ULL) {
                VehicleState state;

                memset(&state, 0, sizeof(state));
                if (config->vehicle_state_getter != NULL) {
                    (void)config->vehicle_state_getter(
                        &state, config->sink_context);
                }

                LOG_INFO("[Replay] time=%llu.%03llu source=%s "
                         "vehicle_speed=%.2f gps_valid=%u can_valid=%u",
                         (unsigned long long)((event.timestamp_us -
                                               first_timestamp_us) /
                                              1000000ULL),
                         (unsigned long long)(((event.timestamp_us -
                                                first_timestamp_us) /
                                               1000ULL) % 1000ULL),
                         replay_source_name(event.source), state.speed_kmh,
                         state.gps_valid ? 1U : 0U,
                         state.can_valid ? 1U : 0U);
                last_report_us = now_us;
            }
        }

        if (!replay_stream_read_next(selected)) {
            selected->has_event = false;
        }
    }

    if (have_last_event_timestamp) {
        replay_emit_vehicle_state_snapshots(config, &snapshot_schedule,
                                            last_event_timestamp_us, true);
    }

    if (!have_first_timestamp) {
        LOG_WARN("[Replay] no valid data rows");
    }
    if (wall_start_us != 0U) {
        LOG_INFO("[Replay] completed");
    }

cleanup:
    if (lappro_ready) {
        lappro_stop(&lappro);
    }
    for (stream_index = 0U; stream_index < REPLAY_STREAM_COUNT;
         ++stream_index) {
        replay_close_stream(&streams[stream_index]);
    }
    replay_discovery_destroy(&discovery);
    return result;
}
