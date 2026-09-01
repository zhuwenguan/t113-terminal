#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "video_capture.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/videodev2.h>

static int video_capture_ioctl(int fd, unsigned long request, void *argument)
{
    int result;

    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result;
}

static uint64_t video_capture_monotonic_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return 0U;
    }
    return (uint64_t)now.tv_sec * 1000000ULL +
           (uint64_t)now.tv_nsec / 1000ULL;
}

void video_capture_close(VideoCapture *capture)
{
    unsigned int index;

    if (capture == NULL) {
        return;
    }
    if (capture->streaming && capture->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        (void)video_capture_ioctl(capture->fd, VIDIOC_STREAMOFF, &type);
    }
    for (index = 0U; index < VIDEO_CAPTURE_BUFFER_COUNT; ++index) {
        if (capture->mapped[index] != NULL &&
            capture->mapped[index] != MAP_FAILED) {
            munmap(capture->mapped[index], capture->mapped_length[index]);
        }
    }
    if (capture->fd >= 0) {
        close(capture->fd);
    }
    memset(capture, 0, sizeof(*capture));
    capture->fd = -1;
}

int video_capture_open(VideoCapture *capture, const char *device_path,
                       uint32_t width, uint32_t height, uint32_t fps)
{
    const char *path = device_path != NULL ? device_path
                                           : VIDEO_CAPTURE_DEFAULT_DEVICE;
    struct v4l2_capability capability;
    struct v4l2_format format;
    struct v4l2_streamparm stream_parameter;
    struct v4l2_requestbuffers request;
    unsigned int index;

    if (capture == NULL || path[0] == '\0' || width == 0U || height == 0U ||
        fps == 0U) {
        errno = EINVAL;
        return -1;
    }
    video_capture_close(capture);
    capture->fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (capture->fd < 0) {
        return -1;
    }
    memset(&capability, 0, sizeof(capability));
    if (video_capture_ioctl(capture->fd, VIDIOC_QUERYCAP, &capability) < 0 ||
        (capability.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0U ||
        (capability.capabilities & V4L2_CAP_STREAMING) == 0U) {
        int saved_errno = errno != 0 ? errno : ENOTSUP;
        video_capture_close(capture);
        errno = saved_errno;
        return -1;
    }

    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = width;
    format.fmt.pix.height = height;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    format.fmt.pix.field = V4L2_FIELD_ANY;
    if (video_capture_ioctl(capture->fd, VIDIOC_S_FMT, &format) < 0 ||
        format.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG ||
        format.fmt.pix.width != width || format.fmt.pix.height != height) {
        int saved_errno = errno != 0 ? errno : ENOTSUP;
        video_capture_close(capture);
        errno = saved_errno;
        return -1;
    }

    memset(&stream_parameter, 0, sizeof(stream_parameter));
    stream_parameter.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    stream_parameter.parm.capture.timeperframe.numerator = 1U;
    stream_parameter.parm.capture.timeperframe.denominator = fps;
    (void)video_capture_ioctl(capture->fd, VIDIOC_S_PARM, &stream_parameter);
    if (stream_parameter.parm.capture.timeperframe.numerator != 0U) {
        fps = stream_parameter.parm.capture.timeperframe.denominator /
              stream_parameter.parm.capture.timeperframe.numerator;
        if (fps == 0U) {
            fps = VIDEO_CAPTURE_FPS;
        }
    }

    memset(&request, 0, sizeof(request));
    request.count = VIDEO_CAPTURE_BUFFER_COUNT;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (video_capture_ioctl(capture->fd, VIDIOC_REQBUFS, &request) < 0 ||
        request.count < 2U || request.count > VIDEO_CAPTURE_BUFFER_COUNT) {
        int saved_errno = errno != 0 ? errno : ENOMEM;
        video_capture_close(capture);
        errno = saved_errno;
        return -1;
    }
    capture->buffer_count = request.count;
    for (index = 0U; index < capture->buffer_count; ++index) {
        struct v4l2_buffer buffer;

        memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        if (video_capture_ioctl(capture->fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            video_capture_close(capture);
            return -1;
        }
        capture->mapped[index] = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE,
                                      MAP_SHARED, capture->fd, buffer.m.offset);
        if (capture->mapped[index] == MAP_FAILED) {
            capture->mapped[index] = NULL;
            video_capture_close(capture);
            return -1;
        }
        capture->mapped_length[index] = buffer.length;
    }
    for (index = 0U; index < capture->buffer_count; ++index) {
        struct v4l2_buffer buffer;

        memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        if (video_capture_ioctl(capture->fd, VIDIOC_QBUF, &buffer) < 0) {
            video_capture_close(capture);
            return -1;
        }
    }
    {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (video_capture_ioctl(capture->fd, VIDIOC_STREAMON, &type) < 0) {
            video_capture_close(capture);
            return -1;
        }
    }
    capture->width = width;
    capture->height = height;
    capture->fps = fps;
    capture->streaming = true;
    return 0;
}

VideoCaptureResult video_capture_dequeue(VideoCapture *capture,
                                         VideoCaptureBuffer *buffer,
                                         int timeout_ms)
{
    struct pollfd descriptor;
    struct v4l2_buffer v4l2_buffer;
    int poll_result;

    if (capture == NULL || buffer == NULL || capture->fd < 0 ||
        !capture->streaming) {
        errno = EINVAL;
        return VIDEO_CAPTURE_DISCONNECTED;
    }
    memset(buffer, 0, sizeof(*buffer));
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.fd = capture->fd;
    descriptor.events = POLLIN;
    do {
        poll_result = poll(&descriptor, 1U, timeout_ms);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result == 0) {
        return VIDEO_CAPTURE_TIMEOUT;
    }
    if (poll_result < 0) {
        return VIDEO_CAPTURE_RETRY;
    }
    if ((descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
        errno = ENODEV;
        return VIDEO_CAPTURE_DISCONNECTED;
    }
    memset(&v4l2_buffer, 0, sizeof(v4l2_buffer));
    v4l2_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    v4l2_buffer.memory = V4L2_MEMORY_MMAP;
    if (video_capture_ioctl(capture->fd, VIDIOC_DQBUF, &v4l2_buffer) < 0) {
        if (errno == EAGAIN || errno == EINTR) {
            return VIDEO_CAPTURE_RETRY;
        }
        if (errno == ENODEV || errno == ENXIO || errno == EPIPE) {
            return VIDEO_CAPTURE_DISCONNECTED;
        }
        return VIDEO_CAPTURE_RETRY;
    }
    if (v4l2_buffer.index >= capture->buffer_count ||
        v4l2_buffer.bytesused == 0U ||
        v4l2_buffer.bytesused > capture->mapped_length[v4l2_buffer.index]) {
        (void)video_capture_queue(capture, v4l2_buffer.index);
        errno = EPROTO;
        return VIDEO_CAPTURE_RETRY;
    }
    buffer->data = capture->mapped[v4l2_buffer.index];
    buffer->size = v4l2_buffer.bytesused;
    buffer->index = v4l2_buffer.index;
    buffer->sequence = v4l2_buffer.sequence;
    buffer->timestamp_us = video_capture_monotonic_us();
    return VIDEO_CAPTURE_FRAME;
}

int video_capture_queue(VideoCapture *capture, unsigned int index)
{
    struct v4l2_buffer buffer;

    if (capture == NULL || capture->fd < 0 || index >= capture->buffer_count) {
        errno = EINVAL;
        return -1;
    }
    memset(&buffer, 0, sizeof(buffer));
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    return video_capture_ioctl(capture->fd, VIDIOC_QBUF, &buffer);
}
