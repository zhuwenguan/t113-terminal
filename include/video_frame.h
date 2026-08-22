#ifndef T113_VIDEO_FRAME_H
#define T113_VIDEO_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIDEO_FRAME_CHANNELS 4U

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    size_t capacity;
    uint8_t *pixels;
    uint64_t timestamp_us;
} VideoFrame;

int video_frame_init(VideoFrame *frame, uint32_t width, uint32_t height);
void video_frame_close(VideoFrame *frame);
bool video_frame_write_ppm(const VideoFrame *frame, const char *path);
bool video_frame_write_bmp(const VideoFrame *frame, const char *path);
bool video_frame_write_image(const VideoFrame *frame, const char *path);

#endif
