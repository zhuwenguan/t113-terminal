#define _POSIX_C_SOURCE 200809L

#include "video_frame.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int video_frame_init(VideoFrame *frame, uint32_t width, uint32_t height)
{
    size_t stride;
    size_t bytes;

    if (frame == NULL || width == 0U || height == 0U) {
        errno = EINVAL;
        return -1;
    }
    if ((uint64_t)width * VIDEO_FRAME_CHANNELS > SIZE_MAX ||
        (uint64_t)width * VIDEO_FRAME_CHANNELS > UINT32_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    stride = (size_t)width * VIDEO_FRAME_CHANNELS;
    if ((size_t)height > SIZE_MAX / stride) {
        errno = EOVERFLOW;
        return -1;
    }
    bytes = stride * (size_t)height;

    memset(frame, 0, sizeof(*frame));
    frame->pixels = calloc(1U, bytes);
    if (frame->pixels == NULL) {
        return -1;
    }
    frame->width = width;
    frame->height = height;
    frame->stride = (uint32_t)stride;
    frame->capacity = bytes;
    return 0;
}

void video_frame_close(VideoFrame *frame)
{
    if (frame == NULL) {
        return;
    }
    free(frame->pixels);
    memset(frame, 0, sizeof(*frame));
}

bool video_frame_write_ppm(const VideoFrame *frame, const char *path)
{
    FILE *file;
    uint32_t row;

    if (frame == NULL || frame->pixels == NULL || path == NULL ||
        frame->width == 0U || frame->height == 0U) {
        errno = EINVAL;
        return false;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    if (fprintf(file, "P6\n%u %u\n255\n", frame->width, frame->height) < 0) {
        fclose(file);
        return false;
    }
    for (row = 0U; row < frame->height; ++row) {
        const uint8_t *source = frame->pixels +
                                (size_t)row * frame->stride;
        uint32_t column;

        for (column = 0U; column < frame->width; ++column) {
            if (fwrite(source + (size_t)column * VIDEO_FRAME_CHANNELS, 1U,
                       3U, file) != 3U) {
                fclose(file);
                return false;
            }
        }
    }
    if (fclose(file) != 0) {
        return false;
    }
    return true;
}

static void video_frame_write_le16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
}

static void video_frame_write_le32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
    destination[3] = (uint8_t)(value >> 24);
}

bool video_frame_write_bmp(const VideoFrame *frame, const char *path)
{
    uint32_t row_bytes;
    uint32_t padding;
    uint64_t image_size;
    uint64_t file_size;
    uint8_t header[54] = { 0 };
    FILE *file;
    uint32_t row;

    if (frame == NULL || frame->pixels == NULL || path == NULL ||
        frame->width == 0U || frame->height == 0U ||
        frame->width > (UINT32_MAX / 3U)) {
        errno = EINVAL;
        return false;
    }
    row_bytes = frame->width * 3U;
    padding = (4U - row_bytes % 4U) % 4U;
    image_size = (uint64_t)(row_bytes + padding) * frame->height;
    file_size = 54U + image_size;
    if (image_size > UINT32_MAX || file_size > UINT32_MAX) {
        errno = EOVERFLOW;
        return false;
    }
    header[0] = 'B';
    header[1] = 'M';
    video_frame_write_le32(header + 2U, (uint32_t)file_size);
    video_frame_write_le32(header + 10U, 54U);
    video_frame_write_le32(header + 14U, 40U);
    video_frame_write_le32(header + 18U, frame->width);
    video_frame_write_le32(header + 22U, frame->height);
    video_frame_write_le16(header + 26U, 1U);
    video_frame_write_le16(header + 28U, 24U);
    video_frame_write_le32(header + 34U, (uint32_t)image_size);
    file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    if (fwrite(header, 1U, sizeof(header), file) != sizeof(header)) {
        fclose(file);
        return false;
    }
    for (row = frame->height; row > 0U; --row) {
        const uint8_t *source = frame->pixels + (size_t)(row - 1U) * frame->stride;
        uint32_t column;
        uint8_t zero_padding[3] = { 0 };

        for (column = 0U; column < frame->width; ++column) {
            const uint8_t *pixel = source + (size_t)column * VIDEO_FRAME_CHANNELS;
            uint8_t bgr[3] = { pixel[2], pixel[1], pixel[0] };
            if (fwrite(bgr, 1U, sizeof(bgr), file) != sizeof(bgr)) {
                fclose(file);
                return false;
            }
        }
        if (padding > 0U && fwrite(zero_padding, 1U, padding, file) != padding) {
            fclose(file);
            return false;
        }
    }
    return fclose(file) == 0;
}

bool video_frame_write_image(const VideoFrame *frame, const char *path)
{
    size_t length;

    if (path == NULL) {
        errno = EINVAL;
        return false;
    }
    length = strlen(path);
    if (length >= 4U &&
        (strcmp(path + length - 4U, ".bmp") == 0 ||
         strcmp(path + length - 4U, ".BMP") == 0)) {
        return video_frame_write_bmp(frame, path);
    }
    return video_frame_write_ppm(frame, path);
}
