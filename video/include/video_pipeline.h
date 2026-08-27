#ifndef T113_VIDEO_PIPELINE_H
#define T113_VIDEO_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>

#include <pthread.h>

#include "hud_renderer.h"
#include "video_capture.h"
#include "video_compositor.h"
#include "video_recorder.h"

typedef bool (*VideoStateSnapshotGetter)(VehicleState *state, void *context);

#define VIDEO_LATENCY_BUCKET_US 100U
#define VIDEO_LATENCY_BUCKET_COUNT 2001U

typedef struct {
    uint64_t samples;
    uint64_t total_us;
    uint64_t max_us;
    uint64_t buckets[VIDEO_LATENCY_BUCKET_COUNT];
} VideoLatencyDistribution;

typedef struct {
    const char *device_path;
    const char *record_path;
    const char *single_frame_path;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t run_seconds;
    bool mock_camera;
    VideoCompositorMode compositor_mode;
    VideoStateSnapshotGetter get_state;
    void *state_context;
} VideoPipelineConfig;

typedef struct {
    uint64_t capture_frames;
    uint64_t rendered_frames;
    uint64_t dropped_frames;
    uint64_t decode_errors;
    uint64_t reconnects;
    uint64_t hud_time_us;
    uint64_t compose_time_us;
    uint64_t yuv_staging_copy_time_us;
    uint64_t yuv_staging_sync_time_us;
    uint64_t yuv_to_argb_time_us;
    uint64_t hud_clear_time_us;
    uint64_t hud_draw_time_us;
    uint64_t hud_sync_time_us;
    uint64_t g2d_blend_time_us;
    uint64_t cedar_handoff_time_us;
    uint64_t decode_time_us;
    uint64_t render_time_us;
    uint64_t first_timestamp_us;
    uint64_t last_timestamp_us;
    uint64_t memory_bytes;
    uint64_t process_cpu_start_us;
    uint64_t process_cpu_end_us;
    VideoCompositorMode active_compositor_mode;
    bool g2d_healthy_at_end;
    uint64_t camera_received_frames;
    uint64_t camera_sequence_lost;
    uint64_t decoded_frames;
    uint64_t processed_frames;
    uint64_t encoded_frames;
    uint64_t enqueued_frames;
    uint64_t written_frames;
    uint64_t application_dropped_frames;
    uint64_t capture_time_us;
    uint64_t decoder_init_time_us;
    uint64_t jpeg_input_copy_time_us;
    uint64_t decoder_buffer_request_time_us;
    uint64_t decoder_submit_time_us;
    uint64_t decoder_decode_time_us;
    uint64_t decoder_request_picture_time_us;
    uint64_t decoder_return_picture_time_us;
    uint64_t decoder_get_frame_time_us;
    uint64_t decoder_release_time_us;
    uint64_t format_convert_time_us;
    uint64_t jpeg_encode_time_us;
    uint64_t encoder_submit_time_us;
    uint64_t encoder_input_acquire_time_us;
    uint64_t encoder_input_copy_time_us;
    uint64_t encoder_cache_flush_time_us;
    uint64_t encoder_input_prepare_time_us;
    uint64_t encoder_hardware_time_us;
    uint64_t encoder_acquire_time_us;
    uint64_t encoder_release_time_us;
    uint64_t encoder_output_bytes;
    uint64_t encoder_output_max_bytes;
    uint64_t mux_write_time_us;
    uint64_t enqueue_copy_time_us;
    uint64_t total_frame_time_us;
    uint64_t frames_over_33ms;
    uint64_t frames_over_50ms;
    uint64_t frames_over_100ms;
    VideoLatencyDistribution latency_decode;
    VideoLatencyDistribution latency_yuv_to_argb;
    VideoLatencyDistribution latency_hud_clear;
    VideoLatencyDistribution latency_hud_draw;
    VideoLatencyDistribution latency_hud_sync;
    VideoLatencyDistribution latency_hud_total;
    VideoLatencyDistribution latency_blend;
    VideoLatencyDistribution latency_cedar_handoff;
    VideoLatencyDistribution latency_jpeg_encode;
    VideoLatencyDistribution latency_mux_write;
    VideoLatencyDistribution latency_total_frame;
    VideoRecorderStats recorder_stats;
    EncodedFrameQueueStats writer_queue_stats;
} VideoPipelineStats;

typedef struct {
    pthread_mutex_t mutex;
    pthread_t thread;
    VideoCapture capture;
    JpegDecoder decoder;
    VideoFrame camera_frame;
    JpegDecodedFrame native_camera_frame;
    HudRenderer hud;
    VideoCompositor compositor;
    VideoRecorder recorder;
    VideoPipelineConfig config;
    VideoPipelineStats stats;
    bool decoder_ready;
    bool recorder_ready;
    bool initialized;
    bool started;
    bool stop_requested;
    bool have_camera_sequence;
    bool g2d_fatal_failure;
    bool video_faulted;
    bool recording_failed;
    uint32_t last_camera_sequence;
} VideoPipeline;

int video_pipeline_init(VideoPipeline *pipeline,
                        const VideoPipelineConfig *config);
int video_pipeline_start(VideoPipeline *pipeline);
void video_pipeline_stop(VideoPipeline *pipeline);
void video_pipeline_print_stats(const VideoPipeline *pipeline);
const VideoPipelineStats *video_pipeline_stats(const VideoPipeline *pipeline);

#endif
