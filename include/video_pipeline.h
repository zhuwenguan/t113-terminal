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

typedef struct {
    const char *device_path;
    const char *record_path;
    const char *single_frame_path;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t run_seconds;
    bool mock_camera;
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
    uint64_t decode_time_us;
    uint64_t render_time_us;
    uint64_t first_timestamp_us;
    uint64_t last_timestamp_us;
    uint64_t memory_bytes;
    uint64_t camera_received_frames;
    uint64_t camera_sequence_lost;
    uint64_t decoded_frames;
    uint64_t processed_frames;
    uint64_t encoded_frames;
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
    uint64_t encoder_hardware_time_us;
    uint64_t encoder_acquire_time_us;
    uint64_t encoder_release_time_us;
    uint64_t encoder_output_bytes;
    uint64_t encoder_output_max_bytes;
    uint64_t mux_write_time_us;
    uint64_t total_frame_time_us;
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
    uint32_t last_camera_sequence;
} VideoPipeline;

int video_pipeline_init(VideoPipeline *pipeline,
                        const VideoPipelineConfig *config);
int video_pipeline_start(VideoPipeline *pipeline);
void video_pipeline_stop(VideoPipeline *pipeline);
void video_pipeline_print_stats(const VideoPipeline *pipeline);
const VideoPipelineStats *video_pipeline_stats(const VideoPipeline *pipeline);

#endif
