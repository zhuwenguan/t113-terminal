#ifndef T113_VIDEO_CAPTURE_H
#define T113_VIDEO_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIDEO_CAPTURE_DEFAULT_DEVICE "/dev/video0"
#define VIDEO_CAPTURE_WIDTH 800U
#define VIDEO_CAPTURE_HEIGHT 600U
#define VIDEO_CAPTURE_FPS 30U
#define VIDEO_CAPTURE_BUFFER_COUNT 3U

typedef enum {
    VIDEO_CAPTURE_TIMEOUT = 0,
    VIDEO_CAPTURE_FRAME = 1,
    VIDEO_CAPTURE_RETRY = -1,
    VIDEO_CAPTURE_DISCONNECTED = -2
} VideoCaptureResult;

typedef struct {
    const uint8_t *data;
    size_t size;
    unsigned int index;
    uint32_t sequence;
    uint64_t timestamp_us;
} VideoCaptureBuffer;

typedef struct {
    int fd;
    void *mapped[VIDEO_CAPTURE_BUFFER_COUNT];
    size_t mapped_length[VIDEO_CAPTURE_BUFFER_COUNT];
    unsigned int buffer_count;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    bool streaming;
} VideoCapture;

int video_capture_open(VideoCapture *capture, const char *device_path,
                       uint32_t width, uint32_t height, uint32_t fps);
VideoCaptureResult video_capture_dequeue(VideoCapture *capture,
                                         VideoCaptureBuffer *buffer,
                                         int timeout_ms);
int video_capture_queue(VideoCapture *capture, unsigned int index);
void video_capture_close(VideoCapture *capture);

#endif
