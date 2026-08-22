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
} VideoPipelineStats;

typedef struct {
    pthread_mutex_t mutex;
    pthread_t thread;
    VideoCapture capture;
    JpegDecoder decoder;
    VideoFrame camera_frame;
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
} VideoPipeline;

int video_pipeline_init(VideoPipeline *pipeline,
                        const VideoPipelineConfig *config);
int video_pipeline_start(VideoPipeline *pipeline);
void video_pipeline_stop(VideoPipeline *pipeline);
void video_pipeline_print_stats(const VideoPipeline *pipeline);
const VideoPipelineStats *video_pipeline_stats(const VideoPipeline *pipeline);

#endif
