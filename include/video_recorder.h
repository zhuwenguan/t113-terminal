#ifndef T113_VIDEO_RECORDER_H
#define T113_VIDEO_RECORDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "jpeg_codec.h"
#include "video_frame.h"

#define VIDEO_RECORDER_DEFAULT_FPS 30U
#define VIDEO_RECORDER_JPEG_CAPACITY (4U * 1024U * 1024U)
#define VIDEO_RECORDER_MAX_DURATION_SECONDS (60U * 60U)

typedef struct {
    uint64_t encoded_frames;
    uint64_t dropped_frames;
    uint64_t bytes_written;
    bool active;
} VideoRecorderStats;

typedef struct {
    uint64_t encode_time_us;
    uint64_t mux_write_time_us;
    uint64_t total_time_us;
    size_t encoded_size;
    bool encoded;
    bool written;
} VideoRecorderFrameTiming;

typedef struct {
    FILE *file;
    FILE *index_file;
    char path[512];
    char index_path[520];
    uint64_t mdat_size_offset;
    uint64_t media_bytes;
    uint64_t frame_count;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    JpegEncoder encoder;
    VideoRecorderStats stats;
    bool initialized;
} VideoRecorder;

int video_recorder_init(VideoRecorder *recorder, const char *path,
                        uint32_t width, uint32_t height, uint32_t fps);
bool video_recorder_write_frame_timed(VideoRecorder *recorder,
                                      const VideoFrame *frame,
                                      VideoRecorderFrameTiming *timing);
bool video_recorder_write_frame(VideoRecorder *recorder,
                                const VideoFrame *frame);
int video_recorder_finalize(VideoRecorder *recorder);
void video_recorder_close(VideoRecorder *recorder);
bool video_recorder_is_recording(const VideoRecorder *recorder);
const VideoRecorderStats *video_recorder_stats(const VideoRecorder *recorder);

#endif
