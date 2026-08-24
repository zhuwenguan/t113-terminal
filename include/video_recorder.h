#ifndef T113_VIDEO_RECORDER_H
#define T113_VIDEO_RECORDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "encoded_frame_queue.h"
#include "jpeg_codec.h"
#include "video_encoder.h"
#include "video_frame.h"

#define VIDEO_RECORDER_DEFAULT_FPS 30U
#define VIDEO_RECORDER_JPEG_CAPACITY (4U * 1024U * 1024U)
#define VIDEO_RECORDER_MAX_DURATION_SECONDS (60U * 60U)

typedef struct {
    uint64_t encoded_frames;
    uint64_t enqueued_frames;
    uint64_t written_frames;
    uint64_t dropped_frames;
    uint64_t encoded_bytes;
    uint64_t enqueued_bytes;
    uint64_t bytes_written;
    uint64_t writer_errors;
    bool writer_failed;
    bool recording_failed;
    bool active;
} VideoRecorderStats;

typedef struct {
    uint64_t encode_time_us;
    uint64_t mux_write_time_us;
    uint64_t enqueue_copy_time_us;
    uint64_t total_time_us;
    uint64_t encoder_submit_time_us;
    uint64_t encoder_input_acquire_time_us;
    uint64_t encoder_input_copy_time_us;
    uint64_t encoder_cache_flush_time_us;
    uint64_t encoder_input_prepare_time_us;
    uint64_t encoder_handoff_time_us;
    uint64_t encoder_hardware_time_us;
    uint64_t encoder_acquire_time_us;
    uint64_t encoder_release_time_us;
    size_t encoded_size;
    bool encoded;
    bool hardware_encoded;
    bool enqueued;
    bool written;
} VideoRecorderFrameTiming;

typedef struct {
    uint64_t offset;
    uint32_t size;
    uint32_t duration;
} VideoRecorderPendingSample;

typedef struct {
    pthread_mutex_t state_mutex;
    pthread_cond_t state_condition;
    pthread_t writer_thread;
    FILE *file;
    FILE *index_file;
    char path[512];
    char index_path[520];
    uint64_t mdat_size_offset;
    uint64_t media_bytes;
    uint64_t frame_count;
    uint64_t producer_frame_index;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint64_t pending_timestamp_us;
    VideoRecorderPendingSample pending_sample;
    EncodedFrameQueue writer_queue;
    JpegEncoder encoder;
    VideoEncoder hardware_encoder;
    bool hardware_encoder_ready;
    bool software_encoder_ready;
    bool argb_hardware_input;
    bool have_pending_sample;
    VideoRecorderStats stats;
    bool state_mutex_initialized;
    bool state_condition_initialized;
    bool writer_thread_started;
    bool writer_ready;
    bool writer_joined;
    bool writer_failed;
    int writer_result;
    bool initialized;
} VideoRecorder;

int video_recorder_init(VideoRecorder *recorder, const char *path,
                        uint32_t width, uint32_t height, uint32_t fps);
int video_recorder_init_argb(VideoRecorder *recorder, const char *path,
                             uint32_t width, uint32_t height, uint32_t fps);
bool video_recorder_write_frame_timed(VideoRecorder *recorder,
                                      const VideoFrame *frame,
                                      uint32_t v4l2_sequence,
                                      VideoRecorderFrameTiming *timing);
bool video_recorder_write_native_frame_timed(VideoRecorder *recorder,
                                             const VideoYuvFrame *frame,
                                             uint64_t timestamp_us,
                                             uint32_t v4l2_sequence,
                                             VideoRecorderFrameTiming *timing);
bool video_recorder_write_argb_frame_timed(VideoRecorder *recorder,
                                           const VideoFrame *frame,
                                           uint64_t timestamp_us,
                                           uint32_t v4l2_sequence,
                                           VideoRecorderFrameTiming *timing);
bool video_recorder_write_frame(VideoRecorder *recorder,
                                const VideoFrame *frame);
bool video_recorder_supports_native_yuv(const VideoRecorder *recorder);
int video_recorder_finalize(VideoRecorder *recorder);
void video_recorder_close(VideoRecorder *recorder);
bool video_recorder_is_recording(const VideoRecorder *recorder);
bool video_recorder_has_failed(const VideoRecorder *recorder);
void video_recorder_get_stats(VideoRecorder *recorder,
                              VideoRecorderStats *stats);
void video_recorder_get_queue_stats(VideoRecorder *recorder,
                                    EncodedFrameQueueStats *stats);

#endif
