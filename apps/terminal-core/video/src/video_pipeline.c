#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "video_pipeline.h"
#include "console_log.h"
#include "resource_monitor.h"

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

static uint64_t video_pipeline_rusage_us(const struct rusage *usage)
{
    if (usage == NULL) {
        return 0U;
    }
    return (uint64_t)usage->ru_utime.tv_sec * 1000000ULL +
           (uint64_t)usage->ru_utime.tv_usec +
           (uint64_t)usage->ru_stime.tv_sec * 1000000ULL +
           (uint64_t)usage->ru_stime.tv_usec;
}

static void video_latency_record(VideoLatencyDistribution *distribution,
                                 uint64_t elapsed_us)
{
    uint64_t bucket;

    if (distribution == NULL) {
        return;
    }
    distribution->samples++;
    distribution->total_us += elapsed_us;
    if (elapsed_us > distribution->max_us) {
        distribution->max_us = elapsed_us;
    }
    bucket = elapsed_us / VIDEO_LATENCY_BUCKET_US;
    if (bucket >= VIDEO_LATENCY_BUCKET_COUNT) {
        bucket = VIDEO_LATENCY_BUCKET_COUNT - 1U;
    }
    distribution->buckets[bucket]++;
}

static uint64_t video_latency_percentile(
    const VideoLatencyDistribution *distribution, unsigned int percentile)
{
    uint64_t target;
    uint64_t cumulative = 0U;
    size_t bucket;

    if (distribution == NULL || distribution->samples == 0U) {
        return 0U;
    }
    target = (distribution->samples * percentile + 99U) / 100U;
    for (bucket = 0U; bucket < VIDEO_LATENCY_BUCKET_COUNT; ++bucket) {
        cumulative += distribution->buckets[bucket];
        if (cumulative >= target) {
            return (uint64_t)bucket * VIDEO_LATENCY_BUCKET_US;
        }
    }
    return distribution->max_us;
}

static void video_latency_log(const char *name,
                              const VideoLatencyDistribution *distribution)
{
    double average_ms = distribution != NULL && distribution->samples > 0U ?
        (double)distribution->total_us / distribution->samples / 1000.0 : 0.0;

    LOG_INFO("Video latency %-14s avg=%.3fms p95=%.3fms p99=%.3fms "
             "max=%.3fms samples=%llu", name, average_ms,
             video_latency_percentile(distribution, 95U) / 1000.0,
             video_latency_percentile(distribution, 99U) / 1000.0,
             distribution != NULL ? distribution->max_us / 1000.0 : 0.0,
             (unsigned long long)(distribution != NULL ?
                                  distribution->samples : 0U));
}

static uint64_t video_writer_latency_percentile(
    const VideoWriterLatencyDistribution *distribution,
    unsigned int percentile)
{
    uint64_t target;
    uint64_t cumulative = 0U;
    size_t bucket;

    if (distribution == NULL || distribution->samples == 0U) {
        return 0U;
    }
    target = (distribution->samples * percentile + 99U) / 100U;
    for (bucket = 0U; bucket < VIDEO_WRITER_LATENCY_BUCKET_COUNT; ++bucket) {
        cumulative += distribution->buckets[bucket];
        if (cumulative >= target) {
            return (uint64_t)bucket * VIDEO_WRITER_LATENCY_BUCKET_US;
        }
    }
    return distribution->max_us;
}

static void video_pipeline_refresh_recorder_stats(VideoPipeline *pipeline)
{
    if (pipeline == NULL || !pipeline->recorder_ready) {
        return;
    }
    video_recorder_get_stats(&pipeline->recorder,
                             &pipeline->stats.recorder_stats);
    video_recorder_get_queue_stats(&pipeline->recorder,
                                   &pipeline->stats.writer_queue_stats);
    pipeline->stats.encoded_frames =
        pipeline->stats.recorder_stats.encoded_frames;
    pipeline->stats.enqueued_frames =
        pipeline->stats.recorder_stats.enqueued_frames;
    pipeline->stats.written_frames =
        pipeline->stats.recorder_stats.written_frames;
    pipeline->stats.mux_write_time_us =
        pipeline->stats.recorder_stats.write_latency.total_us;
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

static void video_pipeline_request_stop(VideoPipeline *pipeline)
{
    if (pipeline != NULL && pthread_mutex_lock(&pipeline->mutex) == 0) {
        pipeline->stop_requested = true;
        pthread_mutex_unlock(&pipeline->mutex);
    }
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

static void video_pipeline_publish_web_frame(VideoPipeline *pipeline,
                                             const VideoCaptureBuffer *buffer)
{
    RaceVideoFrameInfo info;
    uint64_t now_us;
    uint64_t delta_us;
    uint64_t budget_add;
    uint64_t publish_start_us;
    uint32_t publish_fps;
    bool publish;

    if (pipeline == NULL || buffer == NULL) {
        return;
    }
    ++pipeline->video_source_frames_seen;
    publish_fps = pipeline->config.video_publish_fps;
    now_us = buffer->timestamp_us;
    if (!pipeline->video_ipc_ready || publish_fps == 0U || now_us == 0U) {
        pipeline->video_last_source_pts_us = now_us;
        return;
    }

    publish = pipeline->video_last_source_pts_us == 0U;
    if (!publish && now_us >= pipeline->video_last_source_pts_us) {
        delta_us = now_us - pipeline->video_last_source_pts_us;
        budget_add = delta_us > UINT64_MAX / publish_fps ? UINT64_MAX :
                     delta_us * publish_fps;
        if (pipeline->video_rate_budget < 2000000ULL) {
            if (budget_add > 2000000ULL - pipeline->video_rate_budget) {
                pipeline->video_rate_budget = 2000000ULL;
            } else {
                pipeline->video_rate_budget += budget_add;
            }
        }
        publish = pipeline->video_rate_budget >= 1000000ULL;
    }
    pipeline->video_last_source_pts_us = now_us;
    if (!publish) {
        ++pipeline->stats.web_rate_limited_frames;
        return;
    }
    if (pipeline->video_rate_budget >= 1000000ULL) {
        pipeline->video_rate_budget -= 1000000ULL;
    }

    memset(&info, 0, sizeof(info));
    info.pts_us = buffer->timestamp_us;
    info.source_frame_count = pipeline->video_source_frames_seen;
    info.dropped_by_rate_limit = pipeline->stats.web_rate_limited_frames;
    info.source_sequence = buffer->sequence;
    info.width = pipeline->capture.width;
    info.height = pipeline->capture.height;
    publish_start_us = video_pipeline_now_us();
    if (race_video_ipc_publish(&pipeline->video_ipc, buffer->data,
                               buffer->size, &info) < 0) {
        if (errno == EMSGSIZE) {
            ++pipeline->stats.web_oversized_frames;
        }
        LOG_DEBUG("Video web frame publish failed: %s", strerror(errno));
        return;
    }
    pipeline->stats.web_publish_time_us +=
        video_pipeline_now_us() - publish_start_us;
    ++pipeline->stats.web_published_frames;
    pipeline->stats.web_jpeg_bytes += buffer->size;
    if (buffer->size > pipeline->stats.web_jpeg_max_bytes) {
        pipeline->stats.web_jpeg_max_bytes = buffer->size;
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

static void video_pipeline_note_application_drop(VideoPipeline *pipeline)
{
    if (pipeline == NULL) {
        return;
    }
    pipeline->stats.dropped_frames++;
    pipeline->stats.application_dropped_frames++;
}

static void video_pipeline_note_frame_time(VideoPipeline *pipeline,
                                           uint64_t elapsed_us)
{
    pipeline->stats.total_frame_time_us += elapsed_us;
    video_latency_record(&pipeline->stats.latency_total_frame, elapsed_us);
    if (elapsed_us > 33333U) {
        pipeline->stats.frames_over_33ms++;
    }
    if (elapsed_us > 50000U) {
        pipeline->stats.frames_over_50ms++;
    }
    if (elapsed_us > 100000U) {
        pipeline->stats.frames_over_100ms++;
    }
}

static bool video_pipeline_render_frame(VideoPipeline *pipeline,
                                        VideoYuvFrame *native_camera,
                                        uint64_t timestamp_us,
                                        uint32_t v4l2_sequence)
{
    VehicleState state;
    const HudLayer *dynamic_layer = NULL;
    const VideoFrame *record_frame = &pipeline->camera_frame;
    VideoRecorderFrameTiming recorder_timing;
    VideoCompositorTiming compositor_timing;
    HudRenderTiming hud_timing;
    uint64_t render_start = video_pipeline_now_us();
    uint64_t stage_start;
    bool need_rgba_output;
    bool native_record_path = false;
    bool g2d_path;
    bool state_available = video_pipeline_get_state(pipeline, &state);

    (void)state_available;
    memset(&recorder_timing, 0, sizeof(recorder_timing));
    memset(&compositor_timing, 0, sizeof(compositor_timing));
    memset(&hud_timing, 0, sizeof(hud_timing));
    g2d_path = native_camera != NULL &&
               video_compositor_active_mode(&pipeline->compositor) ==
                   VIDEO_COMPOSITOR_MODE_G2D;
    if (g2d_path) {
        dynamic_layer = hud_renderer_render_frame_to_surface(
            &pipeline->hud, &state,
            pipeline->recorder_ready &&
                video_recorder_is_recording(&pipeline->recorder),
            video_compositor_hud_surface(&pipeline->compositor), &hud_timing);
        if (dynamic_layer == NULL ||
            !video_compositor_sync_hud(&pipeline->compositor,
                                       &compositor_timing)) {
            video_pipeline_note_application_drop(pipeline);
            return false;
        }
        record_frame = video_compositor_compose_g2d(
            &pipeline->compositor, native_camera, timestamp_us,
            &compositor_timing);
        if (record_frame == NULL) {
            if (!video_compositor_is_g2d_healthy(&pipeline->compositor)) {
                pipeline->g2d_fatal_failure = pipeline->recorder_ready ||
                    pipeline->config.compositor_mode ==
                        VIDEO_COMPOSITOR_MODE_G2D;
                if (pipeline->g2d_fatal_failure) {
                    LOG_ERROR("Video: fatal G2D failure errno=%d (%s); "
                              "hardware submissions disabled and video "
                              "recording stopped", errno, strerror(errno));
                } else {
                    LOG_ERROR("Video: fatal G2D failure errno=%d (%s); "
                              "hardware submissions disabled, switching to "
                              "CPU compositor", errno, strerror(errno));
                }
            }
            video_pipeline_note_application_drop(pipeline);
            return false;
        }
        pipeline->stats.hud_clear_time_us += hud_timing.clear_time_us;
        pipeline->stats.hud_draw_time_us += hud_timing.draw_time_us;
        pipeline->stats.hud_sync_time_us +=
            compositor_timing.hud_cache_sync_time_us;
        pipeline->stats.hud_time_us += hud_timing.total_time_us +
            compositor_timing.hud_cache_sync_time_us;
        pipeline->stats.yuv_staging_copy_time_us +=
            compositor_timing.yuv_copy_time_us;
        pipeline->stats.yuv_staging_sync_time_us +=
            compositor_timing.yuv_cache_sync_time_us;
        pipeline->stats.yuv_to_argb_time_us +=
            compositor_timing.yuv_to_argb_time_us;
        pipeline->stats.g2d_blend_time_us += compositor_timing.blend_time_us;
        video_latency_record(&pipeline->stats.latency_hud_clear,
                             hud_timing.clear_time_us);
        video_latency_record(&pipeline->stats.latency_hud_draw,
                             hud_timing.draw_time_us);
        video_latency_record(&pipeline->stats.latency_hud_sync,
                             compositor_timing.hud_cache_sync_time_us);
        video_latency_record(&pipeline->stats.latency_hud_total,
                             hud_timing.total_time_us +
                             compositor_timing.hud_cache_sync_time_us);
        video_latency_record(&pipeline->stats.latency_yuv_to_argb,
                             compositor_timing.yuv_to_argb_time_us);
        video_latency_record(&pipeline->stats.latency_blend,
                             compositor_timing.blend_time_us);
        pipeline->stats.compose_time_us +=
            compositor_timing.yuv_copy_time_us +
            compositor_timing.yuv_cache_sync_time_us +
            compositor_timing.yuv_to_argb_time_us +
            compositor_timing.blend_time_us;
    } else {
        if (pipeline->hud.dynamic_surface_only) {
            hud_renderer_close(&pipeline->hud);
            if (hud_renderer_init_style(&pipeline->hud, pipeline->config.width,
                                        pipeline->config.height,
                                        pipeline->config.hud_style,
                                        pipeline->config.hud_font_path,
                                        HUD_PIXEL_FORMAT_RGBA8888) < 0) {
                LOG_ERROR("Video: CPU compositor fallback HUD allocation failed");
                video_pipeline_note_application_drop(pipeline);
                pipeline->g2d_fatal_failure = true;
                return false;
            }
        }
        stage_start = video_pipeline_now_us();
        dynamic_layer = hud_renderer_render_dynamic(
            &pipeline->hud, &state,
            pipeline->recorder_ready &&
                video_recorder_is_recording(&pipeline->recorder));
        {
            uint64_t hud_elapsed = video_pipeline_now_us() - stage_start;

            pipeline->stats.hud_time_us += hud_elapsed;
            video_latency_record(&pipeline->stats.latency_hud_total,
                                 hud_elapsed);
        }
        stage_start = video_pipeline_now_us();
        if (dynamic_layer == NULL ||
            (native_camera != NULL &&
             !video_compositor_compose_yuv(
                 &pipeline->compositor, native_camera,
                 hud_renderer_static_layer(&pipeline->hud), dynamic_layer)) ||
            (native_camera == NULL &&
             !video_compositor_compose(
                 &pipeline->compositor, &pipeline->camera_frame,
                 hud_renderer_static_layer(&pipeline->hud), dynamic_layer,
                 &pipeline->camera_frame))) {
            video_pipeline_note_application_drop(pipeline);
            return false;
        }
        pipeline->stats.compose_time_us += video_pipeline_now_us() - stage_start;
        pipeline->camera_frame.timestamp_us = timestamp_us;
        native_record_path = native_camera != NULL && pipeline->recorder_ready &&
                             video_recorder_supports_native_yuv(
                                 &pipeline->recorder);
    }
    need_rgba_output = !g2d_path && native_camera != NULL &&
        ((pipeline->config.single_frame_path != NULL &&
          pipeline->stats.rendered_frames == 0U) ||
         (pipeline->recorder_ready && !native_record_path));
    if (need_rgba_output) {
        stage_start = video_pipeline_now_us();
        if (!jpeg_decoded_frame_to_rgba(native_camera, &pipeline->camera_frame)) {
            pipeline->stats.format_convert_time_us +=
                video_pipeline_now_us() - stage_start;
            video_pipeline_note_application_drop(pipeline);
            return false;
        }
        pipeline->stats.format_convert_time_us +=
            video_pipeline_now_us() - stage_start;
    }
    if (pipeline->config.single_frame_path != NULL &&
        pipeline->stats.rendered_frames == 0U) {
        const VideoFrame *snapshot_frame = record_frame;

        if (g2d_path) {
            uint32_t row;

            for (row = 0U; row < record_frame->height; ++row) {
                const uint8_t *source = record_frame->pixels +
                    (size_t)row * record_frame->stride;
                uint8_t *destination = pipeline->camera_frame.pixels +
                    (size_t)row * pipeline->camera_frame.stride;
                uint32_t column;

                for (column = 0U; column < record_frame->width; ++column) {
                    destination[column * 4U] = source[column * 4U + 2U];
                    destination[column * 4U + 1U] = source[column * 4U + 1U];
                    destination[column * 4U + 2U] = source[column * 4U];
                    destination[column * 4U + 3U] = source[column * 4U + 3U];
                }
            }
            snapshot_frame = &pipeline->camera_frame;
        }
        if (!video_frame_write_image(snapshot_frame,
                                 pipeline->config.single_frame_path)) {
            LOG_ERROR("Video: cannot write frame %s: %s",
                      pipeline->config.single_frame_path, strerror(errno));
        }
    }
    if (pipeline->recorder_ready &&
        video_recorder_is_recording(&pipeline->recorder) &&
        ((g2d_path &&
          !video_recorder_write_argb_frame_timed(
              &pipeline->recorder, record_frame, timestamp_us,
              v4l2_sequence,
              &recorder_timing)) ||
         (!g2d_path && native_record_path &&
          !video_recorder_write_native_frame_timed(
              &pipeline->recorder, native_camera, timestamp_us,
              v4l2_sequence, &recorder_timing)) ||
         (!g2d_path && !native_record_path &&
          !video_recorder_write_frame_timed(&pipeline->recorder,
                                            &pipeline->camera_frame,
                                            v4l2_sequence,
                                            &recorder_timing)))) {
        video_pipeline_note_application_drop(pipeline);
    }
    if (pipeline->recorder_ready && !pipeline->recording_failed &&
        video_recorder_has_failed(&pipeline->recorder)) {
        pipeline->recording_failed = true;
        LOG_ERROR("Video: storage writer failed; recording stopped while "
                  "video processing and vehicle data remain active");
    }
    if (g2d_path && pipeline->recorder_ready &&
        !pipeline->recorder.hardware_encoder_ready) {
        pipeline->video_faulted = true;
        LOG_ERROR("Video: ARGB Cedar encoder/write failure; stopping video "
                  "module while vehicle data remains active");
    }
    if (pipeline->recorder_ready) {
        pipeline->stats.jpeg_encode_time_us +=
            recorder_timing.hardware_encoded ?
            recorder_timing.encoder_hardware_time_us :
            recorder_timing.encode_time_us;
        pipeline->stats.encoder_submit_time_us +=
            recorder_timing.encoder_submit_time_us;
        pipeline->stats.encoder_input_acquire_time_us +=
            recorder_timing.encoder_input_acquire_time_us;
        pipeline->stats.encoder_input_copy_time_us +=
            recorder_timing.encoder_input_copy_time_us;
        pipeline->stats.encoder_cache_flush_time_us +=
            recorder_timing.encoder_cache_flush_time_us;
        pipeline->stats.encoder_input_prepare_time_us +=
            recorder_timing.encoder_input_prepare_time_us;
        pipeline->stats.cedar_handoff_time_us +=
            recorder_timing.encoder_handoff_time_us;
        if (recorder_timing.encoder_handoff_time_us > 0U) {
            video_latency_record(&pipeline->stats.latency_cedar_handoff,
                                 recorder_timing.encoder_handoff_time_us);
        }
        video_latency_record(&pipeline->stats.latency_jpeg_encode,
            recorder_timing.hardware_encoded ?
            recorder_timing.encoder_hardware_time_us :
            recorder_timing.encode_time_us);
        pipeline->stats.encoder_hardware_time_us +=
            recorder_timing.encoder_hardware_time_us;
        pipeline->stats.encoder_acquire_time_us +=
            recorder_timing.encoder_acquire_time_us;
        pipeline->stats.encoder_release_time_us +=
            recorder_timing.encoder_release_time_us;
        if (recorder_timing.encoded) {
            pipeline->stats.encoder_output_bytes +=
                recorder_timing.encoded_size;
            if (recorder_timing.encoded_size >
                pipeline->stats.encoder_output_max_bytes) {
                pipeline->stats.encoder_output_max_bytes =
                    recorder_timing.encoded_size;
            }
        }
        pipeline->stats.enqueue_copy_time_us +=
            recorder_timing.enqueue_copy_time_us;
        if (recorder_timing.encoded) {
            pipeline->stats.encoded_frames++;
        }
        if (recorder_timing.enqueued) {
            pipeline->stats.enqueued_frames++;
        }
    }
    pipeline->stats.rendered_frames++;
    pipeline->stats.processed_frames++;
    if (pipeline->config.single_frame_path != NULL) {
        video_pipeline_request_stop(pipeline);
    }
    pipeline->stats.render_time_us += video_pipeline_now_us() - render_start;
    if (pipeline->stats.first_timestamp_us == 0U) {
        pipeline->stats.first_timestamp_us = timestamp_us;
    }
    pipeline->stats.last_timestamp_us = timestamp_us;
    return true;
}

static uint64_t video_pipeline_counter_delta(uint64_t current,
                                             uint64_t previous)
{
    return current >= previous ? current - previous : 0U;
}

static double video_pipeline_average_ms(uint64_t elapsed_us,
                                        uint64_t sample_count)
{
    return sample_count > 0U ?
           (double)elapsed_us / (double)sample_count / 1000.0 : 0.0;
}

static void video_pipeline_log_low_frequency_stats(VideoPipeline *pipeline,
                                                   uint64_t now_us,
                                                   uint64_t *next_log_us,
                                                   uint64_t *last_log_us,
                                                   VideoPipelineStats *last_stats)
{
    VideoPipelineStats current;
    uint64_t elapsed_us;
    double elapsed_s;

    if (pipeline == NULL || next_log_us == NULL || last_log_us == NULL ||
        last_stats == NULL || now_us < *next_log_us) {
        return;
    }
    video_pipeline_refresh_recorder_stats(pipeline);
    current = pipeline->stats;
    elapsed_us = now_us > *last_log_us ? now_us - *last_log_us : 0U;
    elapsed_s = (double)elapsed_us / 1000000.0;
    if (elapsed_s > 0.0) {
        LOG_INFO("Video 1s: camera=%llu lost=%llu decoded=%llu processed=%llu "
                 "encoded=%llu enqueued=%llu written=%llu app_drop=%llu "
                 "fps[capture=%.2f decode=%.2f process=%.2f encode=%.2f "
                 "enqueue=%.2f write=%.2f]",
                 (unsigned long long)current.camera_received_frames,
                 (unsigned long long)current.camera_sequence_lost,
                 (unsigned long long)current.decoded_frames,
                 (unsigned long long)current.processed_frames,
                 (unsigned long long)current.encoded_frames,
                 (unsigned long long)current.enqueued_frames,
                 (unsigned long long)current.written_frames,
                 (unsigned long long)current.application_dropped_frames,
                 (double)video_pipeline_counter_delta(
                     current.camera_received_frames,
                     last_stats->camera_received_frames) / elapsed_s,
                 (double)video_pipeline_counter_delta(
                     current.decoded_frames, last_stats->decoded_frames) /
                     elapsed_s,
                 (double)video_pipeline_counter_delta(
                     current.processed_frames, last_stats->processed_frames) /
                     elapsed_s,
                 (double)video_pipeline_counter_delta(
                     current.encoded_frames, last_stats->encoded_frames) /
                     elapsed_s,
                 (double)video_pipeline_counter_delta(
                     current.enqueued_frames, last_stats->enqueued_frames) /
                     elapsed_s,
                 (double)video_pipeline_counter_delta(
                     current.written_frames, last_stats->written_frames) /
                     elapsed_s);
        LOG_INFO("Video profile 1s: capture=%.2fms input_copy=%.2fms "
                 "buffer_request=%.2fms decoder_submit=%.2fms "
                 "decoder_decode=%.2fms request_picture=%.2fms "
                 "return_picture=%.2fms decoder_get_frame=%.2fms "
                 "decoder_release=%.2fms format_convert=%.2fms "
                 "hud=%.2fms compose=%.2fms "
                 "jpeg_encode=%.2fms encoder_submit=%.2fms "
                 "encoder_hw=%.2fms encoder_acquire=%.2fms "
                 "encoder_release=%.2fms enqueue_copy=%.2fms "
                 "processing_total=%.2fms "
                 "encoder_size=%.1fKB "
                 "decoder_init_total=%.2fms",
                 video_pipeline_average_ms(
                     video_pipeline_counter_delta(current.capture_time_us,
                                                  last_stats->capture_time_us),
                     video_pipeline_counter_delta(
                         current.camera_received_frames,
                         last_stats->camera_received_frames)),
                 video_pipeline_average_ms(
                     video_pipeline_counter_delta(
                         current.jpeg_input_copy_time_us,
                         last_stats->jpeg_input_copy_time_us),
                     video_pipeline_counter_delta(current.decoded_frames,
                                                  last_stats->decoded_frames)),
                 video_pipeline_average_ms(
                     video_pipeline_counter_delta(
                         current.decoder_buffer_request_time_us,
                         last_stats->decoder_buffer_request_time_us),
                     video_pipeline_counter_delta(current.decoded_frames,
                                                  last_stats->decoded_frames)),
                 video_pipeline_average_ms(
                     video_pipeline_counter_delta(
                         current.decoder_submit_time_us,
                         last_stats->decoder_submit_time_us),
                     video_pipeline_counter_delta(current.decoded_frames,
                                                  last_stats->decoded_frames)),
                 video_pipeline_average_ms(
                     video_pipeline_counter_delta(
                         current.decoder_decode_time_us,
                         last_stats->decoder_decode_time_us),
                     video_pipeline_counter_delta(current.decoded_frames,
                                                  last_stats->decoded_frames)),
                 video_pipeline_average_ms(
                     video_pipeline_counter_delta(
                         current.decoder_request_picture_time_us,
                         last_stats->decoder_request_picture_time_us),
                     video_pipeline_counter_delta(current.decoded_frames,
                                                  last_stats->decoded_frames)),
                 video_pipeline_average_ms(
                     video_pipeline_counter_delta(
                         current.decoder_return_picture_time_us,
                         last_stats->decoder_return_picture_time_us),
                     video_pipeline_counter_delta(current.decoded_frames,
                                                  last_stats->decoded_frames)),
                 video_pipeline_average_ms(
                     video_pipeline_counter_delta(
                         current.decoder_get_frame_time_us,
                         last_stats->decoder_get_frame_time_us),
                     video_pipeline_counter_delta(current.decoded_frames,
                                                  last_stats->decoded_frames)),
                 video_pipeline_average_ms(
                     video_pipeline_counter_delta(
                         current.decoder_release_time_us,
                         last_stats->decoder_release_time_us),
                     video_pipeline_counter_delta(current.decoded_frames,
                                                  last_stats->decoded_frames)),
                 video_pipeline_average_ms(
                     video_pipeline_counter_delta(
                         current.format_convert_time_us,
                         last_stats->format_convert_time_us),
                     video_pipeline_counter_delta(current.decoded_frames,
                                                  last_stats->decoded_frames)),
                 video_pipeline_average_ms(
                     video_pipeline_counter_delta(current.hud_time_us,
                                                  last_stats->hud_time_us),
                     video_pipeline_counter_delta(current.processed_frames,
                                                  last_stats->processed_frames)),
                 video_pipeline_average_ms(
                     video_pipeline_counter_delta(current.compose_time_us,
                                                  last_stats->compose_time_us),
                     video_pipeline_counter_delta(current.processed_frames,
                                                  last_stats->processed_frames)),
                 video_pipeline_average_ms(
                     video_pipeline_counter_delta(current.jpeg_encode_time_us,
                                                  last_stats->jpeg_encode_time_us),
                     video_pipeline_counter_delta(current.encoded_frames,
                                                  last_stats->encoded_frames)),
                 video_pipeline_average_ms(
                     video_pipeline_counter_delta(
                         current.encoder_submit_time_us,
                         last_stats->encoder_submit_time_us),
                     video_pipeline_counter_delta(current.encoded_frames,
                                                  last_stats->encoded_frames)),
                 video_pipeline_average_ms(
                     video_pipeline_counter_delta(
                         current.encoder_hardware_time_us,
                         last_stats->encoder_hardware_time_us),
                     video_pipeline_counter_delta(current.encoded_frames,
                                                  last_stats->encoded_frames)),
                 video_pipeline_average_ms(
                     video_pipeline_counter_delta(
                         current.encoder_acquire_time_us,
                         last_stats->encoder_acquire_time_us),
                     video_pipeline_counter_delta(current.encoded_frames,
                                                  last_stats->encoded_frames)),
                 video_pipeline_average_ms(
                     video_pipeline_counter_delta(
                         current.encoder_release_time_us,
                         last_stats->encoder_release_time_us),
                     video_pipeline_counter_delta(current.encoded_frames,
                                                  last_stats->encoded_frames)),
                 video_pipeline_average_ms(
                     video_pipeline_counter_delta(
                         current.enqueue_copy_time_us,
                         last_stats->enqueue_copy_time_us),
                     video_pipeline_counter_delta(current.enqueued_frames,
                                                  last_stats->enqueued_frames)),
                 video_pipeline_average_ms(
                     video_pipeline_counter_delta(current.total_frame_time_us,
                                                  last_stats->total_frame_time_us),
                     video_pipeline_counter_delta(current.processed_frames,
                                                  last_stats->processed_frames)),
                 video_pipeline_counter_delta(current.encoded_frames,
                                              last_stats->encoded_frames) > 0U ?
                 (double)video_pipeline_counter_delta(
                     current.encoder_output_bytes,
                     last_stats->encoder_output_bytes) /
                     (double)video_pipeline_counter_delta(
                         current.encoded_frames,
                         last_stats->encoded_frames) / 1024.0 : 0.0,
                 (double)video_pipeline_counter_delta(
                     current.decoder_init_time_us,
                     last_stats->decoder_init_time_us) / 1000.0);
        LOG_INFO("Video G2D profile 1s: yuv_stage_copy=%.2fms "
                 "yuv_stage_sync=%.2fms yuv_to_argb=%.2fms "
                 "hud[clear=%.2f draw=%.2f sync=%.2f] blend=%.2fms "
                 "cedar_handoff=%.2fms",
                 video_pipeline_average_ms(video_pipeline_counter_delta(
                     current.yuv_staging_copy_time_us,
                     last_stats->yuv_staging_copy_time_us),
                     video_pipeline_counter_delta(current.processed_frames,
                                                  last_stats->processed_frames)),
                 video_pipeline_average_ms(video_pipeline_counter_delta(
                     current.yuv_staging_sync_time_us,
                     last_stats->yuv_staging_sync_time_us),
                     video_pipeline_counter_delta(current.processed_frames,
                                                  last_stats->processed_frames)),
                 video_pipeline_average_ms(video_pipeline_counter_delta(
                     current.yuv_to_argb_time_us,
                     last_stats->yuv_to_argb_time_us),
                     video_pipeline_counter_delta(current.processed_frames,
                                                  last_stats->processed_frames)),
                 video_pipeline_average_ms(video_pipeline_counter_delta(
                     current.hud_clear_time_us, last_stats->hud_clear_time_us),
                     video_pipeline_counter_delta(current.processed_frames,
                                                  last_stats->processed_frames)),
                 video_pipeline_average_ms(video_pipeline_counter_delta(
                     current.hud_draw_time_us, last_stats->hud_draw_time_us),
                     video_pipeline_counter_delta(current.processed_frames,
                                                  last_stats->processed_frames)),
                 video_pipeline_average_ms(video_pipeline_counter_delta(
                     current.hud_sync_time_us, last_stats->hud_sync_time_us),
                     video_pipeline_counter_delta(current.processed_frames,
                                                  last_stats->processed_frames)),
                 video_pipeline_average_ms(video_pipeline_counter_delta(
                     current.g2d_blend_time_us, last_stats->g2d_blend_time_us),
                     video_pipeline_counter_delta(current.processed_frames,
                                                  last_stats->processed_frames)),
                 video_pipeline_average_ms(video_pipeline_counter_delta(
                     current.cedar_handoff_time_us,
                     last_stats->cedar_handoff_time_us),
                     video_pipeline_counter_delta(current.encoded_frames,
                                                  last_stats->encoded_frames)));
        if (pipeline->recorder_ready) {
            const VideoRecorderStats *writer = &current.recorder_stats;
            const EncodedFrameQueueStats *queue =
                &current.writer_queue_stats;
            uint64_t interval_write_samples = video_pipeline_counter_delta(
                writer->write_latency.samples,
                last_stats->recorder_stats.write_latency.samples);
            uint64_t interval_write_us = video_pipeline_counter_delta(
                writer->write_latency.total_us,
                last_stats->recorder_stats.write_latency.total_us);

            LOG_INFO("Video writer queue 1s: depth=%zu/%zu avg=%.2f max=%zu "
                     "full=%llu dropped=%llu oversized=%llu",
                     queue->current_depth, queue->capacity,
                     queue->depth_samples > 0U ?
                     (double)queue->depth_sample_total /
                         (double)queue->depth_samples : 0.0,
                     queue->max_depth,
                     (unsigned long long)queue->full_events,
                     (unsigned long long)queue->dropped_frames,
                     (unsigned long long)queue->oversized_frames);
            LOG_INFO("Video writer storage 1s: throughput[encoded=%.2fMB/s "
                     "enqueued=%.2fMB/s written=%.2fMB/s] write_avg=%.2fms "
                     "write_max=%.2fms calls=%llu bytes_per_call=%.1f",
                     (double)video_pipeline_counter_delta(
                         writer->encoded_bytes,
                         last_stats->recorder_stats.encoded_bytes) /
                         elapsed_s / (1024.0 * 1024.0),
                     (double)video_pipeline_counter_delta(
                         writer->enqueued_bytes,
                         last_stats->recorder_stats.enqueued_bytes) /
                         elapsed_s / (1024.0 * 1024.0),
                     (double)video_pipeline_counter_delta(
                         writer->bytes_written,
                         last_stats->recorder_stats.bytes_written) /
                         elapsed_s / (1024.0 * 1024.0),
                     video_pipeline_average_ms(interval_write_us,
                                               interval_write_samples),
                     writer->write_latency.max_us / 1000.0,
                     (unsigned long long)writer->writer_write_calls,
                     writer->writer_write_calls > 0U ?
                     (double)writer->writer_write_call_bytes /
                         (double)writer->writer_write_calls : 0.0);
        }
    }
    *last_stats = current;
    *last_log_us = now_us;
    *next_log_us = now_us + 1000000ULL;
}

static void *video_pipeline_thread(void *context)
{
    VideoPipeline *pipeline = context;
    struct timespec mock_deadline;
    uint64_t mock_tick = 0U;
    uint64_t start_us = video_pipeline_now_us();
    uint64_t next_log_us = start_us + 1000000ULL;
    uint64_t last_log_us = start_us;
    VideoPipelineStats last_log_stats = { 0 };
    uint64_t next_open_us = start_us;
    ResourceMonitor resource_monitor;
    struct rusage usage;

    memset(&usage, 0, sizeof(usage));
    (void)getrusage(RUSAGE_SELF, &usage);
    pipeline->stats.process_cpu_start_us = video_pipeline_rusage_us(&usage);
    resource_monitor_init(&resource_monitor, pipeline->config.fps * 10U);
    resource_monitor_sample(&resource_monitor, 0U, true);
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
            uint64_t frame_start_us;
            bool processed;

            (void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                                  &mock_deadline, NULL);
            if (video_pipeline_stop_requested(pipeline)) {
                break;
            }
            frame_start_us = video_pipeline_now_us();
            video_pipeline_mock_camera(&pipeline->camera_frame, mock_tick++);
            pipeline->camera_frame.timestamp_us = video_pipeline_now_us();
            pipeline->stats.capture_frames++;
            pipeline->stats.camera_received_frames++;
            processed = video_pipeline_render_frame(
                pipeline, NULL, pipeline->camera_frame.timestamp_us,
                (uint32_t)(mock_tick - 1U));
            if (processed) {
                video_pipeline_note_frame_time(
                    pipeline, video_pipeline_now_us() - frame_start_us);
            }
            video_pipeline_add_us(&mock_deadline, interval_us);
        } else {
            VideoCaptureBuffer buffer;
            VideoCaptureResult capture_result;
            uint64_t frame_start_us;

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
                pipeline->have_camera_sequence = false;
                LOG_INFO("Video: V4L2 MJPEG %ux%u @ %u FPS started, buffers=%u",
                         pipeline->capture.width, pipeline->capture.height,
                         pipeline->capture.fps,
                         pipeline->capture.buffer_count);
            }
            frame_start_us = video_pipeline_now_us();
            capture_result = video_capture_dequeue(&pipeline->capture,
                                                   &buffer, 200);
            if (capture_result == VIDEO_CAPTURE_TIMEOUT ||
                capture_result == VIDEO_CAPTURE_RETRY) {
                video_pipeline_log_low_frequency_stats(
                    pipeline, video_pipeline_now_us(), &next_log_us,
                    &last_log_us, &last_log_stats);
                continue;
            }
            if (capture_result == VIDEO_CAPTURE_DISCONNECTED) {
                LOG_WARN("Video: camera disconnected; retaining VehicleState/CAN threads");
                video_capture_close(&pipeline->capture);
                next_open_us = video_pipeline_now_us() + 1000000ULL;
                continue;
            }
            pipeline->stats.capture_frames++;
            pipeline->stats.camera_received_frames++;
            pipeline->stats.capture_time_us +=
                video_pipeline_now_us() - frame_start_us;
            if (pipeline->have_camera_sequence) {
                uint32_t sequence_delta = buffer.sequence -
                                          pipeline->last_camera_sequence;
                if (sequence_delta > 1U && sequence_delta < 0x80000000U) {
                    pipeline->stats.camera_sequence_lost +=
                        (uint64_t)sequence_delta - 1U;
                }
            }
            pipeline->have_camera_sequence = true;
            pipeline->last_camera_sequence = buffer.sequence;
            /* The V4L2 buffer is still owned by this thread. Copy the raw
             * MJPEG into the independent latest-frame IPC before QBUF. This
             * is the only camera-to-web copy and never touches the decoder,
             * compositor, HUD, or recorder path. */
            video_pipeline_publish_web_frame(pipeline, &buffer);
            {
                uint64_t decode_start = video_pipeline_now_us();
                JpegDecoderTiming decoder_timing;
                bool decoded;

                memset(&pipeline->native_camera_frame, 0,
                       sizeof(pipeline->native_camera_frame));
                if (jpeg_decoder_is_native(&pipeline->decoder)) {
                    decoded = jpeg_decoder_decode_native_timed(
                        &pipeline->decoder, buffer.data, buffer.size,
                        &pipeline->native_camera_frame, &decoder_timing);
                } else {
                    decoded = jpeg_decoder_decode_timed(
                        &pipeline->decoder, buffer.data, buffer.size,
                        &pipeline->camera_frame, &decoder_timing);
                }
                pipeline->stats.decode_time_us +=
                    video_pipeline_now_us() - decode_start;
                if (!decoded) {
                    pipeline->stats.decode_errors++;
                    video_pipeline_note_application_drop(pipeline);
                    (void)video_capture_queue(&pipeline->capture, buffer.index);
                    continue;
                }
                pipeline->stats.decoded_frames++;
                pipeline->stats.jpeg_input_copy_time_us +=
                    decoder_timing.input_copy_time_us;
                pipeline->stats.decoder_buffer_request_time_us +=
                    decoder_timing.decoder_buffer_request_time_us;
                pipeline->stats.decoder_submit_time_us +=
                    decoder_timing.decoder_submit_time_us;
                pipeline->stats.decoder_decode_time_us +=
                    decoder_timing.decoder_decode_time_us;
                pipeline->stats.decoder_request_picture_time_us +=
                    decoder_timing.decoder_request_picture_time_us;
                pipeline->stats.decoder_return_picture_time_us +=
                    decoder_timing.decoder_return_picture_time_us;
                pipeline->stats.decoder_get_frame_time_us +=
                    decoder_timing.decoder_get_frame_time_us;
                pipeline->stats.decoder_release_time_us +=
                    decoder_timing.decoder_release_time_us;
                pipeline->stats.format_convert_time_us +=
                    decoder_timing.format_convert_time_us;
                video_latency_record(&pipeline->stats.latency_decode,
                                     decoder_timing.decoder_decode_time_us > 0U ?
                                     decoder_timing.decoder_decode_time_us :
                                     video_pipeline_now_us() - decode_start);
            }
            pipeline->camera_frame.timestamp_us = buffer.timestamp_us;
            (void)video_capture_queue(&pipeline->capture, buffer.index);
            if (video_pipeline_render_frame(
                    pipeline,
                    jpeg_decoder_is_native(&pipeline->decoder) ?
                    &pipeline->native_camera_frame : NULL,
                    buffer.timestamp_us, buffer.sequence)) {
                video_pipeline_note_frame_time(
                    pipeline, video_pipeline_now_us() - frame_start_us);
            }
            if (jpeg_decoder_is_native(&pipeline->decoder) &&
                pipeline->native_camera_frame.opaque != NULL) {
                JpegDecoderTiming return_timing;

                memset(&return_timing, 0, sizeof(return_timing));
                (void)jpeg_decoder_return_native_frame(
                    &pipeline->decoder, &pipeline->native_camera_frame,
                    &return_timing);
                pipeline->stats.decoder_return_picture_time_us +=
                    return_timing.decoder_return_picture_time_us;
            }
            if (pipeline->g2d_fatal_failure || pipeline->video_faulted) {
                break;
            }
        }
        video_pipeline_log_low_frequency_stats(
            pipeline, video_pipeline_now_us(), &next_log_us, &last_log_us,
            &last_log_stats);
        resource_monitor_sample(&resource_monitor,
                                pipeline->stats.processed_frames, false);
    }
    if (pipeline->capture.streaming) {
        video_capture_close(&pipeline->capture);
    }
    resource_monitor_finish(&resource_monitor,
                            pipeline->stats.processed_frames);
    memset(&usage, 0, sizeof(usage));
    (void)getrusage(RUSAGE_SELF, &usage);
    pipeline->stats.process_cpu_end_us = video_pipeline_rusage_us(&usage);
    return NULL;
}

int video_pipeline_init(VideoPipeline *pipeline,
                        const VideoPipelineConfig *config)
{
    VideoCompositorMode compositor_mode;
    int result;

    if (pipeline == NULL || config == NULL || config->width == 0U ||
        config->height == 0U || config->fps == 0U) {
        errno = EINVAL;
        return -1;
    }
    memset(pipeline, 0, sizeof(*pipeline));
    pipeline->capture.fd = -1;
    pipeline->video_ipc.fd = -1;
    pipeline->compositor.g2d_fd = -1;
    pipeline->config = *config;
    if (pipeline->config.video_publish_fps == 0U) {
        pipeline->config.video_publish_fps =
            VIDEO_PIPELINE_DEFAULT_WEB_PUBLISH_FPS;
    }
    compositor_mode = config->compositor_mode;
    result = pthread_mutex_init(&pipeline->mutex, NULL);
    if (result != 0) {
        errno = result;
        return -1;
    }
    pipeline->initialized = true;
    if (!config->mock_camera) {
        const char *video_ipc_name = pipeline->config.video_ipc_name;

        if (video_ipc_name == NULL || video_ipc_name[0] == '\0') {
            video_ipc_name = RACE_VIDEO_IPC_NAME;
        }
        if (race_video_ipc_writer_open(&pipeline->video_ipc,
                                       video_ipc_name) < 0) {
            LOG_WARN("Video web output disabled: cannot open %s: %s",
                     video_ipc_name, strerror(errno));
        } else {
            pipeline->video_ipc_ready = true;
            pipeline->stats.web_ipc_ready = true;
            LOG_INFO("Video web output: IPC=%s target=%u FPS capacity=%u bytes",
                     video_ipc_name, pipeline->config.video_publish_fps,
                     RACE_VIDEO_IPC_MAX_JPEG_SIZE);
        }
    }
    if (video_frame_init(&pipeline->camera_frame, config->width,
                         config->height) < 0) {
        video_pipeline_stop(pipeline);
        errno = ENOMEM;
        return -1;
    }
    if (!config->mock_camera) {
        uint64_t decoder_init_start_us = video_pipeline_now_us();

        if (jpeg_decoder_init(&pipeline->decoder, config->width,
                              config->height) < 0) {
            pipeline->stats.decoder_init_time_us +=
                video_pipeline_now_us() - decoder_init_start_us;
            video_pipeline_stop(pipeline);
            return -1;
        }
        pipeline->stats.decoder_init_time_us +=
            video_pipeline_now_us() - decoder_init_start_us;
        pipeline->decoder_ready = true;
        if (!jpeg_decoder_is_native(&pipeline->decoder) &&
            compositor_mode != VIDEO_COMPOSITOR_MODE_CPU) {
            if (compositor_mode == VIDEO_COMPOSITOR_MODE_G2D) {
                video_pipeline_stop(pipeline);
                errno = ENODEV;
                return -1;
            }
            compositor_mode = VIDEO_COMPOSITOR_MODE_CPU;
            LOG_WARN("Video compositor: native Cedar decoder unavailable; "
                     "using CPU fallback");
        }
    } else if (compositor_mode != VIDEO_COMPOSITOR_MODE_CPU) {
        video_pipeline_stop(pipeline);
        errno = EINVAL;
        return -1;
    }
    /* libawion maps every G2D allocation into the VE IOMMU at palloc time.
     * Keep the native decoder's VE session alive across those allocations;
     * a later first VeInitialize invalidates the allocator's attachments. */
    if (video_compositor_init_mode(&pipeline->compositor, config->width,
                                   config->height, compositor_mode) < 0 ||
        hud_renderer_init_style(
            &pipeline->hud, config->width, config->height, config->hud_style,
            config->hud_font_path,
            video_compositor_active_mode(&pipeline->compositor) ==
                VIDEO_COMPOSITOR_MODE_G2D ? HUD_PIXEL_FORMAT_ARGB8888 :
                                             HUD_PIXEL_FORMAT_RGBA8888) < 0) {
        video_pipeline_stop(pipeline);
        errno = ENOMEM;
        return -1;
    }
    if (config->record_path != NULL && config->record_path[0] != '\0') {
        bool argb_recording = video_compositor_active_mode(
                                  &pipeline->compositor) ==
                              VIDEO_COMPOSITOR_MODE_G2D;
        int recorder_result = argb_recording ?
            video_recorder_init_argb(&pipeline->recorder, config->record_path,
                                     config->width, config->height,
                                     config->fps) :
            video_recorder_init(&pipeline->recorder, config->record_path,
                                config->width, config->height, config->fps);

        if (recorder_result < 0) {
            LOG_WARN("Video: recording disabled because output could not be initialized");
        } else {
            pipeline->recorder_ready = true;
        }
    }
    LOG_INFO("Video compositor: requested=%s active=%s g2d_healthy=%s",
             video_compositor_mode_name(config->compositor_mode),
             video_compositor_mode_name(video_compositor_active_mode(
                 &pipeline->compositor)),
             video_compositor_is_g2d_healthy(&pipeline->compositor) ?
             "yes" : "no");
    LOG_INFO("Video HUD: style=%s resident=%zu bytes",
             hud_style_name(pipeline->hud.style),
             hud_renderer_resident_memory(&pipeline->hud));
    if (config->compositor_mode == VIDEO_COMPOSITOR_MODE_AUTO &&
        video_compositor_active_mode(&pipeline->compositor) ==
            VIDEO_COMPOSITOR_MODE_CPU) {
        LOG_WARN("Video compositor: G2D initialization unavailable; using "
                 "CPU fallback");
    }
    pipeline->stats.active_compositor_mode =
        video_compositor_active_mode(&pipeline->compositor);
    pipeline->stats.g2d_healthy_at_end =
        video_compositor_is_g2d_healthy(&pipeline->compositor);
    pipeline->stats.memory_bytes =
        (uint64_t)pipeline->camera_frame.capacity +
        (uint64_t)config->width * config->height * VIDEO_FRAME_CHANNELS *
            (video_compositor_active_mode(&pipeline->compositor) ==
             VIDEO_COMPOSITOR_MODE_G2D ? 4ULL : 2ULL) +
        (video_compositor_active_mode(&pipeline->compositor) ==
         VIDEO_COMPOSITOR_MODE_G2D ?
         (uint64_t)config->width * config->height * 3ULL / 2ULL : 0ULL) +
             (pipeline->recorder_ready ?
             (uint64_t)VIDEO_WRITER_QUEUE_CAPACITY *
             VIDEO_WRITER_SLOT_CAPACITY : 0ULL);
    pipeline->stats.memory_bytes += hud_renderer_resident_memory(&pipeline->hud);
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
        video_pipeline_request_stop(pipeline);
        pthread_join(pipeline->thread, NULL);
        pipeline->started = false;
    }
    if (pipeline->recorder_ready) {
        (void)video_recorder_finalize(&pipeline->recorder);
        video_pipeline_refresh_recorder_stats(pipeline);
    }
    video_capture_close(&pipeline->capture);
    race_video_ipc_close(&pipeline->video_ipc);
    pipeline->video_ipc_ready = false;
    video_frame_close(&pipeline->camera_frame);
    hud_renderer_close(&pipeline->hud);
    pipeline->stats.g2d_healthy_at_end =
        video_compositor_is_g2d_healthy(&pipeline->compositor);
    /* The allocator's 0x503 teardown must run before the last VE session is
     * destroyed, otherwise the kernel retains the dma-buf attachments. */
    video_compositor_close(&pipeline->compositor);
    if (pipeline->recorder_ready) {
        video_recorder_close(&pipeline->recorder);
        pipeline->recorder_ready = false;
    }
    if (pipeline->decoder_ready) {
        jpeg_decoder_close(&pipeline->decoder);
        pipeline->decoder_ready = false;
    }
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
    double decode_fps = 0.0;
    double process_fps = 0.0;
    double encode_fps = 0.0;
    double enqueue_fps = 0.0;
    double write_fps = 0.0;
    double cpu_percent = 0.0;

    if (pipeline == NULL) {
        return;
    }
    stats = pipeline->stats;
    if (stats.last_timestamp_us > stats.first_timestamp_us) {
        elapsed_s = (double)(stats.last_timestamp_us -
                             stats.first_timestamp_us) / 1000000.0;
        capture_fps = stats.camera_received_frames > 1U ?
            (double)(stats.camera_received_frames - 1U) / elapsed_s : 0.0;
        decode_fps = stats.decoded_frames > 1U ?
            (double)(stats.decoded_frames - 1U) / elapsed_s : 0.0;
        process_fps = stats.processed_frames > 1U ?
            (double)(stats.processed_frames - 1U) / elapsed_s : 0.0;
        encode_fps = stats.encoded_frames > 1U ?
            (double)(stats.encoded_frames - 1U) / elapsed_s : 0.0;
        enqueue_fps = stats.enqueued_frames > 1U ?
            (double)(stats.enqueued_frames - 1U) / elapsed_s : 0.0;
        write_fps = stats.written_frames > 1U ?
            (double)(stats.written_frames - 1U) / elapsed_s : 0.0;
        if (stats.process_cpu_end_us >= stats.process_cpu_start_us) {
            cpu_percent = (double)(stats.process_cpu_end_us -
                                   stats.process_cpu_start_us) /
                          (elapsed_s * 10000.0);
        }
    }
    memset(&usage, 0, sizeof(usage));
    (void)getrusage(RUSAGE_SELF, &usage);
    LOG_INFO("Video statistics: camera=%llu lost=%llu decoded=%llu "
             "processed=%llu encoded=%llu enqueued=%llu written=%llu "
             "app_drop=%llu "
             "capture_fps=%.2f decode_fps=%.2f process_fps=%.2f "
             "encode_fps=%.2f enqueue_fps=%.2f write_fps=%.2f "
             "decode_errors=%llu reconnects=%llu",
             (unsigned long long)stats.camera_received_frames,
             (unsigned long long)stats.camera_sequence_lost,
             (unsigned long long)stats.decoded_frames,
             (unsigned long long)stats.processed_frames,
             (unsigned long long)stats.encoded_frames,
             (unsigned long long)stats.enqueued_frames,
             (unsigned long long)stats.written_frames,
             (unsigned long long)stats.application_dropped_frames,
             capture_fps, decode_fps, process_fps, encode_fps, enqueue_fps,
             write_fps,
             (unsigned long long)stats.decode_errors,
             (unsigned long long)stats.reconnects);
    LOG_INFO("Video web latest-frame: ipc=%s source_frames=%llu published=%llu "
             "rate_limited=%llu oversized=%llu publish_fps=%.2f "
             "jpeg_avg=%.1fKB jpeg_max=%.1fKB copy_avg=%.3fus",
             stats.web_ipc_ready ? "ready" : "disabled",
             (unsigned long long)stats.camera_received_frames,
             (unsigned long long)stats.web_published_frames,
             (unsigned long long)stats.web_rate_limited_frames,
             (unsigned long long)stats.web_oversized_frames,
             elapsed_s > 0.0 ?
             (double)stats.web_published_frames / elapsed_s : 0.0,
             stats.web_published_frames > 0U ?
             (double)stats.web_jpeg_bytes /
                 (double)stats.web_published_frames / 1024.0 : 0.0,
             (double)stats.web_jpeg_max_bytes / 1024.0,
             stats.web_published_frames > 0U ?
             (double)stats.web_publish_time_us /
                 (double)stats.web_published_frames : 0.0);
    LOG_INFO("Video profile: decoder_init_total=%.2fms capture=%.2fms "
             "input_copy=%.2fms buffer_request=%.2fms "
             "decoder_submit=%.2fms decoder_decode=%.2fms "
             "request_picture=%.2fms return_picture=%.2fms "
             "decoder_get_frame=%.2fms decoder_release=%.2fms "
             "format_convert=%.2fms "
             "hud=%.2fms compose=%.2fms jpeg_encode=%.2fms "
             "encoder_submit=%.2fms encoder_hw=%.2fms "
             "encoder_acquire=%.2fms encoder_release=%.2fms "
             "enqueue_copy=%.2fms processing_total=%.2fms "
             "encoder_size_avg=%.1fKB encoder_size_max=%.1fKB "
             "frame_buffers=%lluKB max_rss=%ldKB",
             (double)stats.decoder_init_time_us / 1000.0,
             video_pipeline_average_ms(stats.capture_time_us,
                                        stats.camera_received_frames),
             video_pipeline_average_ms(stats.jpeg_input_copy_time_us,
                                        stats.decoded_frames),
             video_pipeline_average_ms(stats.decoder_buffer_request_time_us,
                                        stats.decoded_frames),
             video_pipeline_average_ms(stats.decoder_submit_time_us,
                                        stats.decoded_frames),
             video_pipeline_average_ms(stats.decoder_decode_time_us,
                                        stats.decoded_frames),
             video_pipeline_average_ms(
                 stats.decoder_request_picture_time_us,
                 stats.decoded_frames),
             video_pipeline_average_ms(stats.decoder_return_picture_time_us,
                                        stats.decoded_frames),
             video_pipeline_average_ms(stats.decoder_get_frame_time_us,
                                        stats.decoded_frames),
             video_pipeline_average_ms(stats.decoder_release_time_us,
                                        stats.decoded_frames),
             video_pipeline_average_ms(stats.format_convert_time_us,
                                        stats.decoded_frames),
             video_pipeline_average_ms(stats.hud_time_us,
                                        stats.processed_frames),
             video_pipeline_average_ms(stats.compose_time_us,
                                        stats.processed_frames),
             video_pipeline_average_ms(stats.jpeg_encode_time_us,
                                        stats.encoded_frames),
             video_pipeline_average_ms(stats.encoder_submit_time_us,
                                        stats.encoded_frames),
             video_pipeline_average_ms(stats.encoder_hardware_time_us,
                                        stats.encoded_frames),
             video_pipeline_average_ms(stats.encoder_acquire_time_us,
                                        stats.encoded_frames),
             video_pipeline_average_ms(stats.encoder_release_time_us,
                                        stats.encoded_frames),
             video_pipeline_average_ms(stats.enqueue_copy_time_us,
                                        stats.enqueued_frames),
             video_pipeline_average_ms(stats.total_frame_time_us,
                                        stats.processed_frames),
             stats.encoded_frames > 0U ?
             (double)stats.encoder_output_bytes /
                 (double)stats.encoded_frames / 1024.0 : 0.0,
             (double)stats.encoder_output_max_bytes / 1024.0,
             (unsigned long long)(stats.memory_bytes / 1024ULL),
             usage.ru_maxrss);
    LOG_INFO("Video G2D profile: mode=%s healthy=%s fatal=%s video_fault=%s "
             "recording_failed=%s "
             "yuv_stage_copy=%.3fms yuv_stage_sync=%.3fms "
             "yuv_to_argb=%.3fms hud[clear=%.3f draw=%.3f sync=%.3f] "
             "blend=%.3fms cedar_handoff=%.3fms process_cpu=%.1f%%",
             video_compositor_mode_name(stats.active_compositor_mode),
             stats.g2d_healthy_at_end ?
             "yes" : "no", pipeline->g2d_fatal_failure ? "yes" : "no",
             pipeline->video_faulted ? "yes" : "no",
             pipeline->recording_failed ? "yes" : "no",
             video_pipeline_average_ms(stats.yuv_staging_copy_time_us,
                                       stats.processed_frames),
             video_pipeline_average_ms(stats.yuv_staging_sync_time_us,
                                       stats.processed_frames),
             video_pipeline_average_ms(stats.yuv_to_argb_time_us,
                                       stats.processed_frames),
             video_pipeline_average_ms(stats.hud_clear_time_us,
                                       stats.processed_frames),
             video_pipeline_average_ms(stats.hud_draw_time_us,
                                       stats.processed_frames),
             video_pipeline_average_ms(stats.hud_sync_time_us,
                                       stats.processed_frames),
             video_pipeline_average_ms(stats.g2d_blend_time_us,
                                       stats.processed_frames),
             video_pipeline_average_ms(stats.cedar_handoff_time_us,
                                       stats.encoded_frames), cpu_percent);
    video_latency_log("mjpeg_decode", &stats.latency_decode);
    video_latency_log("yuv_to_argb", &stats.latency_yuv_to_argb);
    video_latency_log("hud_clear", &stats.latency_hud_clear);
    video_latency_log("hud_draw", &stats.latency_hud_draw);
    video_latency_log("hud_sync", &stats.latency_hud_sync);
    video_latency_log("hud_total", &stats.latency_hud_total);
    video_latency_log("g2d_blend", &stats.latency_blend);
    video_latency_log("cedar_handoff", &stats.latency_cedar_handoff);
    video_latency_log("jpeg_encode", &stats.latency_jpeg_encode);
    video_latency_log("total_frame", &stats.latency_total_frame);
    LOG_INFO("Video writer queue: depth=%zu/%zu avg=%.2f max=%zu full=%llu "
             "dropped=%llu oversized=%llu encoded=%llu enqueued=%llu "
             "written=%llu",
             stats.writer_queue_stats.current_depth,
             stats.writer_queue_stats.capacity,
             stats.writer_queue_stats.depth_samples > 0U ?
             (double)stats.writer_queue_stats.depth_sample_total /
                 (double)stats.writer_queue_stats.depth_samples : 0.0,
             stats.writer_queue_stats.max_depth,
             (unsigned long long)stats.writer_queue_stats.full_events,
             (unsigned long long)stats.writer_queue_stats.dropped_frames,
             (unsigned long long)stats.writer_queue_stats.oversized_frames,
             (unsigned long long)stats.recorder_stats.encoded_frames,
             (unsigned long long)stats.recorder_stats.enqueued_frames,
             (unsigned long long)stats.recorder_stats.written_frames);
    LOG_INFO("Video writer latency: avg=%.3fms p95=%.3fms p99=%.3fms "
             "max=%.3fms samples=%llu >50ms=%llu >100ms=%llu >200ms=%llu "
             ">500ms=%llu calls=%llu bytes_per_call=%.1f "
             "throughput[encoded=%.2fMB/s enqueued=%.2fMB/s written=%.2fMB/s] "
             "failed=%s errors=%llu",
             stats.recorder_stats.write_latency.samples > 0U ?
             (double)stats.recorder_stats.write_latency.total_us /
                 (double)stats.recorder_stats.write_latency.samples / 1000.0 :
                 0.0,
             video_writer_latency_percentile(
                 &stats.recorder_stats.write_latency, 95U) / 1000.0,
             video_writer_latency_percentile(
                 &stats.recorder_stats.write_latency, 99U) / 1000.0,
             stats.recorder_stats.write_latency.max_us / 1000.0,
             (unsigned long long)stats.recorder_stats.write_latency.samples,
             (unsigned long long)stats.recorder_stats.writes_over_50ms,
             (unsigned long long)stats.recorder_stats.writes_over_100ms,
             (unsigned long long)stats.recorder_stats.writes_over_200ms,
             (unsigned long long)stats.recorder_stats.writes_over_500ms,
             (unsigned long long)stats.recorder_stats.writer_write_calls,
             stats.recorder_stats.writer_write_calls > 0U ?
             (double)stats.recorder_stats.writer_write_call_bytes /
                 (double)stats.recorder_stats.writer_write_calls : 0.0,
             elapsed_s > 0.0 ?
             (double)stats.recorder_stats.encoded_bytes / elapsed_s /
                 (1024.0 * 1024.0) : 0.0,
             elapsed_s > 0.0 ?
             (double)stats.recorder_stats.enqueued_bytes / elapsed_s /
                 (1024.0 * 1024.0) : 0.0,
             elapsed_s > 0.0 ?
             (double)stats.recorder_stats.bytes_written / elapsed_s /
                 (1024.0 * 1024.0) : 0.0,
             stats.recorder_stats.writer_failed ? "yes" : "no",
             (unsigned long long)stats.recorder_stats.writer_errors);
    LOG_INFO("Video frame budget: >33.3ms=%llu (%.3f%%) >50ms=%llu "
             "(%.3f%%) >100ms=%llu (%.3f%%)",
             (unsigned long long)stats.frames_over_33ms,
             stats.latency_total_frame.samples > 0U ?
             (double)stats.frames_over_33ms * 100.0 /
                 stats.latency_total_frame.samples : 0.0,
             (unsigned long long)stats.frames_over_50ms,
             stats.latency_total_frame.samples > 0U ?
             (double)stats.frames_over_50ms * 100.0 /
                 stats.latency_total_frame.samples : 0.0,
             (unsigned long long)stats.frames_over_100ms,
             stats.latency_total_frame.samples > 0U ?
             (double)stats.frames_over_100ms * 100.0 /
                 stats.latency_total_frame.samples : 0.0);
}

const VideoPipelineStats *video_pipeline_stats(const VideoPipeline *pipeline)
{
    return pipeline != NULL ? &pipeline->stats : NULL;
}
