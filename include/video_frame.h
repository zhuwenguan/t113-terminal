#ifndef T113_VIDEO_FRAME_H
#define T113_VIDEO_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIDEO_FRAME_CHANNELS 4U

typedef enum {
    VIDEO_YUV_FORMAT_UNKNOWN = 0,
    VIDEO_YUV_FORMAT_YUV420P = 1,
    VIDEO_YUV_FORMAT_YV12 = 2,
    VIDEO_YUV_FORMAT_NV21 = 3,
    VIDEO_YUV_FORMAT_NV12 = 4
} VideoYuvPixelFormat;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    size_t capacity;
    uint8_t *pixels;
    uint64_t timestamp_us;
} VideoFrame;

/* A decoded camera frame remains in the vendor-owned native YUV picture
 * until the compositor has finished with it. */
typedef struct {
    VideoYuvPixelFormat format;
    uint32_t width;
    uint32_t height;
    uint32_t stride[3];
    uint32_t left_offset;
    uint32_t top_offset;
    uint8_t *plane[3];
    void *opaque;
} VideoYuvFrame;

int video_frame_init(VideoFrame *frame, uint32_t width, uint32_t height);
void video_frame_close(VideoFrame *frame);
bool video_frame_write_ppm(const VideoFrame *frame, const char *path);
bool video_frame_write_bmp(const VideoFrame *frame, const char *path);
bool video_frame_write_image(const VideoFrame *frame, const char *path);

#endif
