#define _POSIX_C_SOURCE 200809L

#include "state_reader.h"
#include "webd_config.h"
#include "webd_json.h"

#include <civetweb.h>

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <race/console_log.h>
#include <race/time_utils.h>

#define WEBD_MAX_WS_CLIENTS 8U
#define WEBD_JSON_BUFFER_SIZE 8192U
#define WEBD_SERVER_THREADS 8U
#define WEBD_CLIENT_SEND_TIMEOUT_MS 1000U

typedef struct WebdRuntime WebdRuntime;

typedef struct {
    WebdRuntime *runtime;
    struct mg_connection *connection;
    pthread_t thread;
    bool active;
    bool thread_started;
    bool thread_done;
    bool stop;
    bool reaping;
    bool send_in_progress;
    uint64_t send_started_us;
} WebdClient;

struct WebdRuntime {
    WebdConfig config;
    StateReader reader;
    pthread_mutex_t state_mutex;
    StateSnapshot latest;
    char latest_json[WEBD_JSON_BUFFER_SIZE];
    size_t latest_json_length;
    bool have_latest;
    pthread_t state_thread;
    bool state_thread_started;

    pthread_mutex_t clients_mutex;
    WebdClient clients[WEBD_MAX_WS_CLIENTS];
    unsigned int client_count;
    bool stopping;

    struct mg_context *server;
    uint64_t start_mono_us;
};

static volatile sig_atomic_t webd_stop_requested;

static void *websocket_sender_thread(void *context);
static int webd_log_message(const struct mg_connection *connection,
                            const char *message);
static int webd_log_access(const struct mg_connection *connection,
                           const char *message);

static uint64_t monotonic_us(void)
{
    uint64_t timestamp_us = 0U;

    if (race_monotonic_us(&timestamp_us) < 0) {
        return 0U;
    }
    return timestamp_us;
}

static void handle_signal(int signal_number)
{
    (void)signal_number;
    webd_stop_requested = 1;
}

static int install_signals(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        return -1;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGPIPE, &action, NULL) < 0) {
        return -1;
    }
    return 0;
}

static bool runtime_is_stopping(WebdRuntime *runtime)
{
    bool stopping;

    pthread_mutex_lock(&runtime->clients_mutex);
    stopping = runtime->stopping;
    pthread_mutex_unlock(&runtime->clients_mutex);
    return stopping;
}

static bool client_should_stop(WebdClient *client)
{
    bool stop;

    pthread_mutex_lock(&client->runtime->clients_mutex);
    stop = client->stop || client->runtime->stopping;
    pthread_mutex_unlock(&client->runtime->clients_mutex);
    return stop;
}

static bool cache_get(WebdRuntime *runtime, uint64_t now_mono_us,
                      StateSnapshot *snapshot)
{
    bool available;

    pthread_mutex_lock(&runtime->state_mutex);
    available = runtime->have_latest;
    if (available && snapshot != NULL) {
        *snapshot = runtime->latest;
    }
    pthread_mutex_unlock(&runtime->state_mutex);

    if (available && snapshot != NULL && snapshot->published_mono_us != 0U) {
        snapshot->fresh = !race_shared_state_is_stale(
            now_mono_us, snapshot->published_mono_us, RACE_STATE_STALE_US);
        if (now_mono_us >= snapshot->published_mono_us) {
            snapshot->age_us = now_mono_us - snapshot->published_mono_us;
        }
    }
    return available;
}

static void cache_publish(WebdRuntime *runtime, const StateSnapshot *snapshot,
                          const char *json, size_t json_length)
{
    pthread_mutex_lock(&runtime->state_mutex);
    runtime->latest = *snapshot;
    runtime->have_latest = true;
    if (json != NULL && json_length < sizeof(runtime->latest_json)) {
        memcpy(runtime->latest_json, json, json_length);
        runtime->latest_json[json_length] = '\0';
        runtime->latest_json_length = json_length;
    } else {
        runtime->latest_json[0] = '\0';
        runtime->latest_json_length = 0U;
    }
    pthread_mutex_unlock(&runtime->state_mutex);
}

static void cache_mark_unavailable(WebdRuntime *runtime, uint64_t now_mono_us)
{
    StateSnapshot snapshot;
    bool have_snapshot;

    pthread_mutex_lock(&runtime->state_mutex);
    have_snapshot = runtime->have_latest;
    snapshot = runtime->latest;
    pthread_mutex_unlock(&runtime->state_mutex);
    if (!have_snapshot) {
        return;
    }
    snapshot.fresh = false;
    if (snapshot.published_mono_us != 0U &&
        now_mono_us >= snapshot.published_mono_us) {
        snapshot.age_us = now_mono_us - snapshot.published_mono_us;
    }
    cache_publish(runtime, &snapshot, NULL, 0U);
}

static void enforce_client_send_deadlines(WebdRuntime *runtime,
                                          uint64_t now_mono_us)
{
    struct mg_connection *connections[WEBD_MAX_WS_CLIENTS];
    unsigned int count = 0U;
    unsigned int index;

    pthread_mutex_lock(&runtime->clients_mutex);
    for (index = 0U; index < WEBD_MAX_WS_CLIENTS; ++index) {
        WebdClient *client = &runtime->clients[index];
        if (client->active && client->send_in_progress &&
            client->send_started_us != 0U &&
            now_mono_us >= client->send_started_us &&
            now_mono_us - client->send_started_us >=
                (uint64_t)WEBD_CLIENT_SEND_TIMEOUT_MS * 1000ULL) {
            client->stop = true;
            connections[count++] = client->connection;
            LOG_WARN("webd: closing slow WebSocket client after %u ms",
                     WEBD_CLIENT_SEND_TIMEOUT_MS);
        }
    }
    pthread_mutex_unlock(&runtime->clients_mutex);
    for (index = 0U; index < count; ++index) {
        if (connections[index] != NULL) {
            mg_close_connection(connections[index]);
        }
    }
}

static void sleep_until_us(uint64_t deadline_us)
{
    struct timespec deadline;
    int result;

    deadline.tv_sec = (time_t)(deadline_us / 1000000ULL);
    deadline.tv_nsec = (long)((deadline_us % 1000000ULL) * 1000ULL);
    do {
        result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                                 &deadline, NULL);
    } while (result == EINTR && !webd_stop_requested);
}

static void *state_cache_thread_main(void *context)
{
    WebdRuntime *runtime = context;
    const uint64_t period_us = 1000000ULL / runtime->config.telemetry_rate_hz;
    uint64_t deadline_us = monotonic_us();

    if (deadline_us == 0U) {
        deadline_us = 1U;
    }
    while (!webd_stop_requested && !runtime_is_stopping(runtime)) {
        StateSnapshot snapshot;
        char json[WEBD_JSON_BUFFER_SIZE];
        int json_length;
        uint64_t now_mono_us = monotonic_us();

        if (now_mono_us == 0U) {
            now_mono_us = deadline_us;
        }
        if (state_reader_read(&runtime->reader, now_mono_us, &snapshot) == 0) {
            json_length = snapshot.fresh ?
                webd_json_state(json, sizeof(json), &snapshot) : -1;
            cache_publish(runtime, &snapshot,
                          json_length >= 0 ? json : NULL,
                          json_length >= 0 ? (size_t)json_length : 0U);
        } else {
            cache_mark_unavailable(runtime, now_mono_us);
        }
        enforce_client_send_deadlines(runtime, now_mono_us);

        deadline_us += period_us;
        now_mono_us = monotonic_us();
        if (now_mono_us != 0U && deadline_us <= now_mono_us) {
            deadline_us = now_mono_us + period_us;
        }
        sleep_until_us(deadline_us);
    }
    return NULL;
}

static const char *status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Error";
    }
}

static int send_http_response(struct mg_connection *connection, int status,
                              const char *content_type, const char *body,
                              size_t body_length, bool head_request)
{
    int result;

    result = mg_printf(
        connection,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",
        status, status_text(status), content_type, body_length);
    if (result < 0) {
        return -1;
    }
    if (!head_request && body_length > 0U && body != NULL) {
        result = mg_write(connection, body, body_length);
        if (result != (int)body_length) {
            return -1;
        }
    }
    return status;
}

static int handle_api_state(WebdRuntime *runtime,
                            struct mg_connection *connection,
                            bool head_request)
{
    StateSnapshot snapshot;
    char body[WEBD_JSON_BUFFER_SIZE];
    int body_length;

    if (!cache_get(runtime, monotonic_us(), &snapshot) || !snapshot.fresh) {
        body_length = webd_json_state_error(
            body, sizeof(body), "vehicle_state_unavailable");
        return send_http_response(connection, 503, "application/json; charset=utf-8",
                                  body, (size_t)body_length, head_request);
    }
    body_length = webd_json_state(body, sizeof(body), &snapshot);
    if (body_length < 0) {
        body_length = webd_json_state_error(body, sizeof(body), "json_encode_failed");
        return send_http_response(connection, 500, "application/json; charset=utf-8",
                                  body, (size_t)body_length, head_request);
    }
    return send_http_response(connection, 200, "application/json; charset=utf-8",
                              body, (size_t)body_length, head_request);
}

static int handle_api_system(WebdRuntime *runtime,
                             struct mg_connection *connection,
                             bool head_request)
{
    StateSnapshot snapshot;
    bool have_snapshot;
    uint64_t now_mono_us = monotonic_us();
    uint64_t uptime_s = now_mono_us >= runtime->start_mono_us ?
        (now_mono_us - runtime->start_mono_us) / 1000000ULL : 0U;
    unsigned int client_count;
    char body[1024];
    int body_length;

    have_snapshot = cache_get(runtime, now_mono_us, &snapshot);
    pthread_mutex_lock(&runtime->clients_mutex);
    client_count = runtime->client_count;
    pthread_mutex_unlock(&runtime->clients_mutex);
    body_length = webd_json_system(body, sizeof(body), uptime_s,
                                   have_snapshot ? &snapshot : NULL,
                                   client_count, now_mono_us);
    if (body_length < 0) {
        body_length = webd_json_state_error(body, sizeof(body), "json_encode_failed");
        return send_http_response(connection, 500, "application/json; charset=utf-8",
                                  body, (size_t)body_length, head_request);
    }
    return send_http_response(connection, 200, "application/json; charset=utf-8",
                              body, (size_t)body_length, head_request);
}

static int webd_begin_request(struct mg_connection *connection)
{
    const struct mg_request_info *request_info;
    WebdRuntime *runtime;
    bool head_request;

    request_info = mg_get_request_info(connection);
    runtime = mg_get_user_context_data(connection);
    if (request_info == NULL || runtime == NULL ||
        request_info->local_uri == NULL || request_info->request_method == NULL) {
        return 0;
    }
    head_request = strcmp(request_info->request_method, "HEAD") == 0;
    if (strcmp(request_info->request_method, "GET") != 0 && !head_request) {
        if (strncmp(request_info->local_uri, "/api/", 5U) == 0) {
            static const char body[] = "{\"error\":\"method_not_allowed\"}";
            (void)send_http_response(connection, 405, "application/json; charset=utf-8",
                                     body, sizeof(body) - 1U, false);
            return 405;
        }
        return 0;
    }
    if (strcmp(request_info->local_uri, "/api/state") == 0) {
        return handle_api_state(runtime, connection, head_request);
    }
    if (strcmp(request_info->local_uri, "/api/system") == 0) {
        return handle_api_system(runtime, connection, head_request);
    }
    if (strcmp(request_info->local_uri, "/api/capabilities") == 0) {
        char body[256];
        int body_length = webd_json_capabilities(body, sizeof(body));
        if (body_length < 0) {
            return 500;
        }
        return send_http_response(connection, 200,
                                  "application/json; charset=utf-8", body,
                                  (size_t)body_length, head_request);
    }
    if (strncmp(request_info->local_uri, "/api/", 5U) == 0) {
        static const char body[] = "{\"error\":\"not_found\"}";
        (void)send_http_response(connection, 404, "application/json; charset=utf-8",
                                 body, sizeof(body) - 1U, head_request);
        return 404;
    }
    /* CivetWeb serves the configured dashboard document root. */
    return 0;
}

static int websocket_connect(const struct mg_connection *connection,
                             void *context)
{
    WebdRuntime *runtime = context;
    int reject = 0;

    (void)connection;
    pthread_mutex_lock(&runtime->clients_mutex);
    if (runtime->stopping || runtime->client_count >= WEBD_MAX_WS_CLIENTS) {
        reject = 1;
    }
    pthread_mutex_unlock(&runtime->clients_mutex);
    return reject;
}

static void websocket_ready(struct mg_connection *connection, void *context)
{
    WebdRuntime *runtime = context;
    WebdClient *client = NULL;
    unsigned int index;
    int result;

    pthread_mutex_lock(&runtime->clients_mutex);
    for (index = 0U; index < WEBD_MAX_WS_CLIENTS; ++index) {
        if (!runtime->clients[index].active) {
            client = &runtime->clients[index];
            memset(client, 0, sizeof(*client));
            client->runtime = runtime;
            client->connection = connection;
            client->active = true;
            ++runtime->client_count;
            break;
        }
    }
    pthread_mutex_unlock(&runtime->clients_mutex);

    if (client == NULL) {
        mg_close_connection(connection);
        return;
    }
    result = pthread_create(&client->thread, NULL,
                            websocket_sender_thread, client);
    if (result != 0) {
        pthread_mutex_lock(&runtime->clients_mutex);
        client->stop = true;
        client->active = false;
        if (runtime->client_count > 0U) {
            --runtime->client_count;
        }
        pthread_mutex_unlock(&runtime->clients_mutex);
        LOG_WARN("webd: cannot start WebSocket sender: %s", strerror(result));
        mg_close_connection(connection);
        return;
    }
    pthread_mutex_lock(&runtime->clients_mutex);
    client->thread_started = true;
    pthread_mutex_unlock(&runtime->clients_mutex);
}

static int websocket_data(struct mg_connection *connection, int bits,
                          char *data, size_t data_length, void *context)
{
    int opcode = bits & 0x0f;

    (void)connection;
    (void)data;
    (void)data_length;
    (void)context;
    return opcode == MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE ? 0 : 1;
}

static void websocket_close(const struct mg_connection *connection,
                            void *context)
{
    WebdRuntime *runtime = context;
    unsigned int index;

    pthread_mutex_lock(&runtime->clients_mutex);
    for (index = 0U; index < WEBD_MAX_WS_CLIENTS; ++index) {
        WebdClient *client = &runtime->clients[index];
        if (client->active && client->connection == connection) {
            client->stop = true;
            break;
        }
    }
    pthread_mutex_unlock(&runtime->clients_mutex);
}

static void *websocket_sender_thread(void *context)
{
    WebdClient *client = context;
    WebdRuntime *runtime = client->runtime;
    const uint64_t period_us = 1000000ULL / runtime->config.telemetry_rate_hz;
    uint64_t deadline_us = monotonic_us();
    uint32_t last_sequence = 0U;

    if (deadline_us == 0U) {
        deadline_us = 1U;
    }
    while (!webd_stop_requested && !client_should_stop(client)) {
        char json[WEBD_JSON_BUFFER_SIZE];
        size_t json_length = 0U;
        uint32_t sequence = 0U;
        bool have_json = false;

        deadline_us += period_us;
        sleep_until_us(deadline_us);
        if (webd_stop_requested || client_should_stop(client)) {
            break;
        }
        pthread_mutex_lock(&runtime->state_mutex);
        if (runtime->have_latest && runtime->latest_json_length > 0U &&
            runtime->latest.sequence != last_sequence) {
            json_length = runtime->latest_json_length;
            memcpy(json, runtime->latest_json, json_length + 1U);
            sequence = runtime->latest.sequence;
            have_json = true;
        }
        pthread_mutex_unlock(&runtime->state_mutex);
        if (!have_json) {
            continue;
        }
        pthread_mutex_lock(&runtime->clients_mutex);
        if (client->stop || runtime->stopping) {
            pthread_mutex_unlock(&runtime->clients_mutex);
            break;
        }
        client->send_in_progress = true;
        client->send_started_us = monotonic_us();
        pthread_mutex_unlock(&runtime->clients_mutex);
        if (mg_websocket_write(client->connection, MG_WEBSOCKET_OPCODE_TEXT,
                               json, json_length) <= 0) {
            pthread_mutex_lock(&runtime->clients_mutex);
            client->send_in_progress = false;
            client->send_started_us = 0U;
            client->stop = true;
            pthread_mutex_unlock(&runtime->clients_mutex);
            mg_close_connection(client->connection);
            break;
        }
        pthread_mutex_lock(&runtime->clients_mutex);
        client->send_in_progress = false;
        client->send_started_us = 0U;
        pthread_mutex_unlock(&runtime->clients_mutex);
        last_sequence = sequence;
    }
    pthread_mutex_lock(&runtime->clients_mutex);
    client->thread_done = true;
    pthread_mutex_unlock(&runtime->clients_mutex);
    return NULL;
}

static void reap_clients(WebdRuntime *runtime)
{
    unsigned int index;

    for (index = 0U; index < WEBD_MAX_WS_CLIENTS; ++index) {
        pthread_t thread;
        WebdClient *client = &runtime->clients[index];
        bool reap = false;

        pthread_mutex_lock(&runtime->clients_mutex);
        if (client->active && client->thread_started && client->thread_done &&
            !client->reaping) {
            client->reaping = true;
            thread = client->thread;
            reap = true;
        }
        pthread_mutex_unlock(&runtime->clients_mutex);
        if (reap) {
            (void)pthread_join(thread, NULL);
            pthread_mutex_lock(&runtime->clients_mutex);
            client->active = false;
            client->thread_started = false;
            client->thread_done = false;
            client->reaping = false;
            client->connection = NULL;
            if (runtime->client_count > 0U) {
                --runtime->client_count;
            }
            pthread_mutex_unlock(&runtime->clients_mutex);
        }
    }
}

static void stop_clients(WebdRuntime *runtime)
{
    struct mg_connection *connections[WEBD_MAX_WS_CLIENTS];
    unsigned int count = 0U;
    unsigned int index;

    pthread_mutex_lock(&runtime->clients_mutex);
    runtime->stopping = true;
    for (index = 0U; index < WEBD_MAX_WS_CLIENTS; ++index) {
        if (runtime->clients[index].active) {
            runtime->clients[index].stop = true;
            connections[count++] = runtime->clients[index].connection;
        }
    }
    pthread_mutex_unlock(&runtime->clients_mutex);
    for (index = 0U; index < count; ++index) {
        if (connections[index] != NULL) {
            mg_close_connection(connections[index]);
        }
    }
}

static void join_clients(WebdRuntime *runtime)
{
    unsigned int index;

    for (index = 0U; index < WEBD_MAX_WS_CLIENTS; ++index) {
        pthread_t thread;
        bool join = false;

        pthread_mutex_lock(&runtime->clients_mutex);
        if (runtime->clients[index].active &&
            runtime->clients[index].thread_started) {
            runtime->clients[index].reaping = true;
            thread = runtime->clients[index].thread;
            join = true;
        }
        pthread_mutex_unlock(&runtime->clients_mutex);
        if (join) {
            (void)pthread_join(thread, NULL);
            pthread_mutex_lock(&runtime->clients_mutex);
            runtime->clients[index].active = false;
            runtime->clients[index].thread_started = false;
            runtime->clients[index].connection = NULL;
            pthread_mutex_unlock(&runtime->clients_mutex);
        }
    }
    runtime->client_count = 0U;
}

static int make_listening_port(const WebdConfig *config, char *buffer,
                               size_t buffer_size)
{
    int written;

    if (strchr(config->listen_address, ':') != NULL &&
        config->listen_address[0] != '[') {
        written = snprintf(buffer, buffer_size, "[%s]:%u",
                           config->listen_address, config->listen_port);
    } else {
        written = snprintf(buffer, buffer_size, "%s:%u",
                           config->listen_address, config->listen_port);
    }
    return written < 0 || (size_t)written >= buffer_size ? -1 : 0;
}

static int check_web_root(const char *path)
{
    struct stat status;

    if (stat(path, &status) < 0) {
        return -1;
    }
    if (!S_ISDIR(status.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

static int start_server(WebdRuntime *runtime)
{
    struct mg_callbacks callbacks;
    char listening_port[128];
    char thread_count[16];
    const char *options[24];
    unsigned int option = 0U;

    if (make_listening_port(&runtime->config, listening_port,
                            sizeof(listening_port)) < 0) {
        errno = EOVERFLOW;
        return -1;
    }
    (void)snprintf(thread_count, sizeof(thread_count), "%u",
                   WEBD_SERVER_THREADS);
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.begin_request = webd_begin_request;
    callbacks.log_message = webd_log_message;
    callbacks.log_access = webd_log_access;
    options[option++] = "listening_ports";
    options[option++] = listening_port;
    options[option++] = "document_root";
    options[option++] = runtime->config.web_root;
    options[option++] = "index_files";
    options[option++] = "index.html";
    options[option++] = "enable_directory_listing";
    options[option++] = "no";
    options[option++] = "num_threads";
    options[option++] = thread_count;
    options[option++] = "enable_keep_alive";
    options[option++] = "no";
    options[option++] = "request_timeout_ms";
    options[option++] = "3000";
    options[option++] = "websocket_timeout_ms";
    options[option++] = "5000";
    options[option++] = "enable_websocket_ping_pong";
    options[option++] = "yes";
    options[option++] = "tcp_nodelay";
    options[option++] = "yes";
    options[option++] = "access_control_allow_origin";
    options[option++] = "*";
    options[option] = NULL;
    runtime->server = mg_start(&callbacks, runtime, options);
    if (runtime->server == NULL) {
        return -1;
    }
    mg_set_websocket_handler(runtime->server, "/ws/telemetry",
                             websocket_connect, websocket_ready,
                             websocket_data, websocket_close, runtime);
    LOG_INFO("webd listening on %s:%u, web_root=%s, rate=%u Hz",
             runtime->config.listen_address, runtime->config.listen_port,
             runtime->config.web_root, runtime->config.telemetry_rate_hz);
    return 0;
}

static int webd_log_message(const struct mg_connection *connection,
                            const char *message)
{
    (void)connection;
    if (message != NULL) {
        LOG_DEBUG("civetweb: %s", message);
    }
    return 1;
}

static int webd_log_access(const struct mg_connection *connection,
                           const char *message)
{
    (void)connection;
    (void)message;
    return 1;
}

int main(int argc, char **argv)
{
    WebdRuntime runtime;
    const char *config_path = NULL;
    int result;
    unsigned int initialized_features;
    bool state_mutex_initialized = false;
    bool clients_mutex_initialized = false;

    memset(&runtime, 0, sizeof(runtime));
    webd_config_defaults(&runtime.config);
    console_log_init();
    {
        int index;
        for (index = 1; index < argc; ++index) {
            if (strcmp(argv[index], "--verbose") == 0 ||
                strcmp(argv[index], "-v") == 0) {
                console_log_set_level(CONSOLE_LOG_DEBUG);
            }
        }
    }
    result = webd_config_parse_arguments(&runtime.config, argc, argv,
                                         &config_path);
    if (result == 1) {
        webd_config_print_usage(argv[0]);
        return EXIT_SUCCESS;
    }
    if (result < 0) {
        webd_config_print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    /* Command-line options have precedence over file values. */
    webd_config_defaults(&runtime.config);
    if (config_path != NULL && webd_config_load_file(&runtime.config,
                                                      config_path) < 0) {
        fprintf(stderr, "webd: cannot read config %s: %s\n", config_path,
                strerror(errno));
        return EXIT_FAILURE;
    }
    result = webd_config_parse_arguments(&runtime.config, argc, argv, NULL);
    if (result == 1) {
        webd_config_print_usage(argv[0]);
        return EXIT_SUCCESS;
    }
    if (result < 0) {
        webd_config_print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (check_web_root(runtime.config.web_root) < 0) {
        fprintf(stderr, "webd: web root %s is not a directory: %s\n",
                runtime.config.web_root, strerror(errno));
        return EXIT_FAILURE;
    }
    if (install_signals() < 0) {
        fprintf(stderr, "webd: cannot install signal handlers: %s\n",
                strerror(errno));
        return EXIT_FAILURE;
    }
    runtime.start_mono_us = monotonic_us();
    state_reader_init(&runtime.reader, runtime.config.state_shm_name);
    result = pthread_mutex_init(&runtime.state_mutex, NULL);
    if (result == 0) {
        state_mutex_initialized = true;
        result = pthread_mutex_init(&runtime.clients_mutex, NULL);
    }
    if (result != 0) {
        fprintf(stderr, "webd: cannot initialize mutexes\n");
        if (state_mutex_initialized) {
            pthread_mutex_destroy(&runtime.state_mutex);
        }
        state_reader_close(&runtime.reader);
        return EXIT_FAILURE;
    }
    clients_mutex_initialized = true;
    initialized_features = mg_init_library(MG_FEATURES_FILES |
                                           MG_FEATURES_WEBSOCKET);
    if ((initialized_features & MG_FEATURES_WEBSOCKET) == 0U) {
        LOG_ERROR("webd: CivetWeb WebSocket feature is unavailable");
        if (clients_mutex_initialized) {
            pthread_mutex_destroy(&runtime.clients_mutex);
        }
        if (state_mutex_initialized) {
            pthread_mutex_destroy(&runtime.state_mutex);
        }
        state_reader_close(&runtime.reader);
        return EXIT_FAILURE;
    }
    if (start_server(&runtime) < 0) {
        LOG_ERROR("webd: cannot start HTTP server on %s:%u: %s",
                  runtime.config.listen_address, runtime.config.listen_port,
                  strerror(errno));
        (void)mg_exit_library();
        if (clients_mutex_initialized) {
            pthread_mutex_destroy(&runtime.clients_mutex);
        }
        if (state_mutex_initialized) {
            pthread_mutex_destroy(&runtime.state_mutex);
        }
        state_reader_close(&runtime.reader);
        return EXIT_FAILURE;
    }
    result = pthread_create(&runtime.state_thread, NULL,
                            state_cache_thread_main, &runtime);
    if (result != 0) {
        LOG_ERROR("webd: cannot start state reader: %s", strerror(result));
        webd_stop_requested = 1;
    } else {
        runtime.state_thread_started = true;
    }

    while (!webd_stop_requested) {
        struct timespec delay = {0, 100000000L};
        reap_clients(&runtime);
        (void)nanosleep(&delay, NULL);
    }
    stop_clients(&runtime);
    if (runtime.server != NULL) {
        mg_stop(runtime.server);
        runtime.server = NULL;
    }
    if (runtime.state_thread_started) {
        (void)pthread_join(runtime.state_thread, NULL);
    }
    join_clients(&runtime);
    state_reader_close(&runtime.reader);
    (void)mg_exit_library();
    pthread_mutex_destroy(&runtime.clients_mutex);
    pthread_mutex_destroy(&runtime.state_mutex);
    LOG_INFO("webd stopped");
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
