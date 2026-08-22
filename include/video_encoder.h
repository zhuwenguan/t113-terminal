#ifndef T113_VIDEO_ENCODER_H
#define T113_VIDEO_ENCODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cedar_vencoder_abi.h"
#include "video_frame.h"

#define VIDEO_ENCODER_DEFAULT_JPEG_QUALITY 82

typedef struct {
    CedarVideoEncCreate create;
    CedarVideoEncDestroy destroy;
    CedarVideoEncInit init;
    CedarVideoEncUnInit uninit;
    CedarAllocInputBuffer alloc_input;
    CedarGetOneAllocInputBuffer get_input;
    CedarFlushCacheAllocInputBuffer flush_input;
    CedarReturnOneAllocInputBuffer return_input;
    CedarReleaseAllocInputBuffer release_input;
    CedarAddOneInputBuffer add_input;
    CedarVideoEncodeOneFrame encode;
    CedarAlreadyUsedInputBuffer used_input;
    CedarGetOneBitstreamFrame get_output;
    CedarFreeOneBitstreamFrame free_output;
    CedarVideoEncSetParameter set_parameter;
} VideoEncoderApi;

typedef struct {
    uint64_t submit_time_us;
    uint64_t hardware_encode_time_us;
    uint64_t acquire_time_us;
    uint64_t release_time_us;
    uint64_t total_time_us;
    size_t output_size;
    bool encoded;
} VideoEncoderTiming;

typedef struct {
    void *library_handle;
    void *jpeg_library_handle;
    void *encoder;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t aligned_height;
    size_t output_capacity;
    uint8_t *output;
    bool initialized;
    bool input_buffers_allocated;
    bool available;
    VideoEncoderApi api;
    struct {
        uint8_t *pAddrVirY;
        uint8_t *pAddrVirC;
    } input_memory;
} VideoEncoder;

int video_encoder_init(VideoEncoder *encoder, uint32_t width,
                       uint32_t height, uint32_t fps, int quality,
                       size_t output_capacity);
void video_encoder_close(VideoEncoder *encoder);
bool video_encoder_is_available(const VideoEncoder *encoder);
bool video_encoder_encode_yuv_timed(VideoEncoder *encoder,
                                    const VideoYuvFrame *frame,
                                    const uint8_t **jpeg_data,
                                    size_t *jpeg_size,
                                    VideoEncoderTiming *timing);
bool video_encoder_encode_yuv(VideoEncoder *encoder, const VideoYuvFrame *frame,
                              const uint8_t **jpeg_data, size_t *jpeg_size);

#endif
