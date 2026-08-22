#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "video_pipeline.h"
#include "console_log.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

static uint64_t video_pipeline_now_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return 0U;
    }
    return (uint64_t)now.tv_sec * 1000000ULL +
           (uint64_t)now.tv_nsec / 1000ULL;
}

static bool video_pipeline_stop_requested(VideoPipeline *pipeline)
{
    bool requested;

    if (pthread_mutex_lock(&pipeline->mutex) != 0) {
        return true;
    }
    requested = pipeline->stop_requested;
    pthread_mutex_unlock(&pipeline->mutex);
    return requested;
}

static void video_pipeline_add_us(struct timespec *deadline, uint64_t us)
{
    deadline->tv_sec += (time_t)(us / 1000000ULL);
    deadline->tv_nsec += (long)((us % 1000000ULL) * 1000ULL);
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
}

static void video_pipeline_mock_camera(VideoFrame *frame, uint64_t tick)
{
    uint32_t row;

    for (row = 0U; row < frame->height; ++row) {
        uint8_t *pixels = frame->pixels + (size_t)row * frame->stride;
        uint32_t column;

        for (column = 0U; column < frame->width; ++column) {
            uint8_t *pixel = pixels + (size_t)column * VIDEO_FRAME_CHANNELS;
            pixel[0] = (uint8_t)((column + tick * 2U) & 0xffU);
            pixel[1] = (uint8_t)((row / 2U + tick) & 0xffU);
            pixel[2] = (uint8_t)(((column + row) / 4U + tick * 3U) & 0xffU);
            pixel[3] = 255U;
        }
    }
    /* A few high-contrast road-like guide lines make layout testing easier. */
    for (row = 0U; row < frame->height; row += 90U) {
        uint32_t column;
        for (column = 0U; column < frame->width; ++column) {
            uint8_t *pixel = frame->pixels + (size_t)row * frame->stride +
                             (size_t)column * VIDEO_FRAME_CHANNELS;
            pixel[0] = 35U;
            pixel[1] = 35U;
            pixel[2] = 35U;
        }
    }
}

static bool video_pipeline_get_state(VideoPipeline *pipeline,
                                     VehicleState *state)
{
    memset(state, 0, sizeof(*state));
    if (pipeline->config.get_state != NULL &&
        pipeline->config.get_state(state, pipeline->config.state_context)) {
        return true;
    }
    return false;
}

static bool video_pipeline_render_frame(VideoPipeline *pipeline,
                                        uint64_t timestamp_us)
{
    VehicleState state;
    const HudLayer *dynamic_layer;
    uint64_t render_start = video_pipeline_now_us();
    uint64_t stage_start;
    bool state_available = video_pipeline_get_state(pipeline, &state);

    (void)state_available;
    stage_start = video_pipeline_now_us();
    dynamic_layer = hud_renderer_render_dynamic(
        &pipeline->hud, &state,
        pipeline->recorder_ready &&
            video_recorder_is_recording(&pipeline->recorder));
    pipeline->stats.hud_time_us += video_pipeline_now_us() - stage_start;
    stage_start = video_pipeline_now_us();
    if (dynamic_layer == NULL ||
        !video_compositor_compose(&pipeline->compositor,
                                  &pipeline->camera_frame,
                                  hud_renderer_static_layer(&pipeline->hud),
                                  dynamic_layer, &pipeline->camera_frame)) {
        pipeline->stats.dropped_frames++;
        return false;
    }
    pipeline->stats.compose_time_us += video_pipeline_now_us() - stage_start;
    pipeline->camera_frame.timestamp_us = timestamp_us;
    if (pipeline->config.single_frame_path != NULL &&
        pipeline->stats.rendered_frames == 0U &&
        !video_frame_write_image(&pipeline->camera_frame,
                                 pipeline->config.single_frame_path)) {
        LOG_ERROR("Video: cannot write frame %s: %s",
                  pipeline->config.single_frame_path, strerror(errno));
    }
    if (pipeline->recorder_ready &&
        !video_recorder_write_frame(&pipeline->recorder,
                                    &pipeline->camera_frame)) {
        pipeline->stats.dropped_frames++;
    }
    pipeline->stats.rendered_frames++;
    if (pipeline->config.single_frame_path != NULL) {
        pipeline->stop_requested = true;
    }
    pipeline->stats.render_time_us += video_pipeline_now_us() - render_start;
    if (pipeline->stats.first_timestamp_us == 0U) {
        pipeline->stats.first_timestamp_us = timestamp_us;
    }
    pipeline->stats.last_timestamp_us = timestamp_us;
    return true;
}

static void video_pipeline_log_low_frequency_stats(VideoPipeline *pipeline,
                                                   uint64_t now_us,
                                                   uint64_t *next_log_us)
{
    if (now_us < *next_log_us) {
        return;
    }
    *next_log_us = now_us + 5000000ULL;
    LOG_INFO("Video stats: capture=%llu render=%llu dropped=%llu decode_errors=%llu",
             (unsigned long long)pipeline->stats.capture_frames,
             (unsigned long long)pipeline->stats.rendered_frames,
             (unsigned long long)pipeline->stats.dropped_frames,
             (unsigned long long)pipeline->stats.decode_errors);
}

static void *video_pipeline_thread(void *context)
{
    VideoPipeline *pipeline = context;
    struct timespec mock_deadline;
    uint64_t mock_tick = 0U;
    uint64_t start_us = video_pipeline_now_us();
    uint64_t next_log_us = start_us + 5000000ULL;
    uint64_t next_open_us = start_us;

    (void)clock_gettime(CLOCK_MONOTONIC, &mock_deadline);
    while (!video_pipeline_stop_requested(pipeline)) {
        uint64_t now_us = video_pipeline_now_us();

        if (pipeline->config.run_seconds > 0U && start_us > 0U &&
            now_us >= start_us + (uint64_t)pipeline->config.run_seconds *
                                  1000000ULL) {
            break;
        }
        if (pipeline->config.mock_camera) {
            uint64_t interval_us = 1000000ULL / pipeline->config.fps;

            (void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                                  &mock_deadline, NULL);
            if (video_pipeline_stop_requested(pipeline)) {
                break;
            }
            video_pipeline_mock_camera(&pipeline->camera_frame, mock_tick++);
            pipeline->camera_frame.timestamp_us = video_pipeline_now_us();
            pipeline->stats.capture_frames++;
            (void)video_pipeline_render_frame(pipeline,
                                               pipeline->camera_frame.timestamp_us);
            video_pipeline_add_us(&mock_deadline, interval_us);
        } else {
            VideoCaptureBuffer buffer;
            VideoCaptureResult capture_result;

            if (!pipeline->capture.streaming) {
                if (now_us < next_open_us) {
                    struct timespec wait_time = { 0, 100000000L };
                    (void)nanosleep(&wait_time, NULL);
                    continue;
                }
                if (video_capture_open(&pipeline->capture,
                                       pipeline->config.device_path,
                                       pipeline->config.width,
                                       pipeline->config.height,
                                       pipeline->config.fps) < 0) {
                    next_open_us = now_us + 1000000ULL;
                    LOG_WARN("Video: cannot open MJPEG camera %s: %s",
                             pipeline->config.device_path != NULL ?
                             pipeline->config.device_path :
                             VIDEO_CAPTURE_DEFAULT_DEVICE,
                             strerror(errno));
                    continue;
                }
                pipeline->stats.reconnects++;
                LOG_INFO("Video: V4L2 MJPEG %ux%u @ %u FPS started",
                         pipeline->capture.width, pipeline->capture.height,
                         pipeline->capture.fps);
            }
            capture_result = video_capture_dequeue(&pipeline->capture,
                                                   &buffer, 200);
            if (capture_result == VIDEO_CAPTURE_TIMEOUT ||
                capture_result == VIDEO_CAPTURE_RETRY) {
                video_pipeline_log_low_frequency_stats(pipeline,
                                                       video_pipeline_now_us(),
                                                       &next_log_us);
                continue;
            }
            if (capture_result == VIDEO_CAPTURE_DISCONNECTED) {
                LOG_WARN("Video: camera disconnected; retaining VehicleState/CAN threads");
                video_capture_close(&pipeline->capture);
                next_open_us = video_pipeline_now_us() + 1000000ULL;
                continue;
            }
            pipeline->stats.capture_frames++;
            {
                uint64_t decode_start = video_pipeline_now_us();
                bool decoded = jpeg_decoder_decode(&pipeline->decoder,
                                                   buffer.data, buffer.size,
                                                   &pipeline->camera_frame);
                pipeline->stats.decode_time_us +=
                    video_pipeline_now_us() - decode_start;
                if (!decoded) {
                    pipeline->stats.decode_errors++;
                    pipeline->stats.dropped_frames++;
                    (void)video_capture_queue(&pipeline->capture, buffer.index);
                    continue;
                }
            }
            pipeline->camera_frame.timestamp_us = buffer.timestamp_us;
            (void)video_capture_queue(&pipeline->capture, buffer.index);
            (void)video_pipeline_render_frame(pipeline, buffer.timestamp_us);
        }
        video_pipeline_log_low_frequency_stats(pipeline, video_pipeline_now_us(),
                                               &next_log_us);
    }
    if (pipeline->capture.streaming) {
        video_capture_close(&pipeline->capture);
    }
    return NULL;
}

int video_pipeline_init(VideoPipeline *pipeline,
                        const VideoPipelineConfig *config)
{
    int result;

    if (pipeline == NULL || config == NULL || config->width == 0U ||
        config->height == 0U || config->fps == 0U ||
        ((uint64_t)config->width * HUD_BASE_HEIGHT) !=
        ((uint64_t)config->height * HUD_BASE_WIDTH)) {
        errno = EINVAL;
        return -1;
    }
    memset(pipeline, 0, sizeof(*pipeline));
    pipeline->capture.fd = -1;
    pipeline->config = *config;
    result = pthread_mutex_init(&pipeline->mutex, NULL);
    if (result != 0) {
        errno = result;
        return -1;
    }
    pipeline->initialized = true;
    if (video_frame_init(&pipeline->camera_frame, config->width,
                         config->height) < 0 ||
        hud_renderer_init(&pipeline->hud, config->width, config->height) < 0 ||
        video_compositor_init(&pipeline->compositor, config->width,
                              config->height) < 0) {
        video_pipeline_stop(pipeline);
        errno = ENOMEM;
        return -1;
    }
    if (!config->mock_camera) {
        if (jpeg_decoder_init(&pipeline->decoder, config->width) < 0) {
            video_pipeline_stop(pipeline);
            return -1;
        }
        pipeline->decoder_ready = true;
    }
    if (config->record_path != NULL && config->record_path[0] != '\0') {
        if (video_recorder_init(&pipeline->recorder, config->record_path,
                                config->width, config->height, config->fps) < 0) {
            LOG_WARN("Video: recording disabled because output could not be initialized");
        } else {
            pipeline->recorder_ready = true;
        }
    }
    pipeline->stats.memory_bytes =
        (uint64_t)pipeline->camera_frame.capacity +
        (uint64_t)config->width * config->height * VIDEO_FRAME_CHANNELS * 2ULL;
    return 0;
}

int video_pipeline_start(VideoPipeline *pipeline)
{
    int result;

    if (pipeline == NULL || !pipeline->initialized || pipeline->started) {
        errno = EINVAL;
        return -1;
    }
    result = pthread_create(&pipeline->thread, NULL, video_pipeline_thread,
                            pipeline);
    if (result != 0) {
        errno = result;
        return -1;
    }
    pipeline->started = true;
    return 0;
}

void video_pipeline_stop(VideoPipeline *pipeline)
{
    if (pipeline == NULL) {
        return;
    }
    if (pipeline->initialized && pipeline->started) {
        if (pthread_mutex_lock(&pipeline->mutex) == 0) {
            pipeline->stop_requested = true;
            pthread_mutex_unlock(&pipeline->mutex);
        }
        pthread_join(pipeline->thread, NULL);
        pipeline->started = false;
    }
    if (pipeline->recorder_ready) {
        (void)video_recorder_finalize(&pipeline->recorder);
        video_recorder_close(&pipeline->recorder);
        pipeline->recorder_ready = false;
    }
    video_capture_close(&pipeline->capture);
    if (pipeline->decoder_ready) {
        jpeg_decoder_close(&pipeline->decoder);
        pipeline->decoder_ready = false;
    }
    video_frame_close(&pipeline->camera_frame);
    hud_renderer_close(&pipeline->hud);
    if (pipeline->initialized) {
        pthread_mutex_destroy(&pipeline->mutex);
    }
    pipeline->initialized = false;
}

void video_pipeline_print_stats(const VideoPipeline *pipeline)
{
    VideoPipelineStats stats;
    struct rusage usage;
    double elapsed_s = 0.0;
    double capture_fps = 0.0;
    double render_fps = 0.0;
    double average_render_ms = 0.0;
    double average_hud_ms = 0.0;
    double average_compose_ms = 0.0;
    double average_decode_ms = 0.0;

    if (pipeline == NULL) {
        return;
    }
    stats = pipeline->stats;
    if (stats.last_timestamp_us > stats.first_timestamp_us) {
        elapsed_s = (double)(stats.last_timestamp_us -
                             stats.first_timestamp_us) / 1000000.0;
        capture_fps = (double)stats.capture_frames / elapsed_s;
        render_fps = (double)stats.rendered_frames / elapsed_s;
    }
    if (stats.rendered_frames > 0U) {
        average_render_ms = (double)stats.render_time_us /
                            (double)stats.rendered_frames / 1000.0;
        average_hud_ms = (double)stats.hud_time_us /
                         (double)stats.rendered_frames / 1000.0;
        average_compose_ms = (double)stats.compose_time_us /
                             (double)stats.rendered_frames / 1000.0;
        average_decode_ms = (double)stats.decode_time_us /
                            (double)stats.capture_frames / 1000.0;
    }
    memset(&usage, 0, sizeof(usage));
    (void)getrusage(RUSAGE_SELF, &usage);
    LOG_INFO("Video statistics: capture_fps=%.2f render_fps=%.2f dropped=%llu "
             "decode_errors=%llu reconnects=%llu average_render_ms=%.2f "
             "decode_ms=%.2f hud_ms=%.2f compose_ms=%.2f "
             "frame_buffers=%lluKB max_rss=%ldKB",
             capture_fps, render_fps,
             (unsigned long long)stats.dropped_frames,
             (unsigned long long)stats.decode_errors,
             (unsigned long long)stats.reconnects, average_render_ms,
             average_decode_ms, average_hud_ms, average_compose_ms,
             (unsigned long long)(stats.memory_bytes / 1024ULL),
             usage.ru_maxrss);
}

const VideoPipelineStats *video_pipeline_stats(const VideoPipeline *pipeline)
{
    return pipeline != NULL ? &pipeline->stats : NULL;
}
