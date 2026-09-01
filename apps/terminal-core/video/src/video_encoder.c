#define _POSIX_C_SOURCE 200809L

#include "video_encoder.h"

#include "cedar_vencoder_abi.h"
#include "console_log.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__arm__) || defined(__aarch64__)
static uint64_t video_encoder_now_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return 0U;
    }
    return (uint64_t)now.tv_sec * 1000000ULL +
           (uint64_t)now.tv_nsec / 1000ULL;
}

static bool video_encoder_load_symbol(void *library, void *target,
                                      size_t target_size, const char *name)
{
    void *symbol;

    if (library == NULL || target == NULL || target_size != sizeof(symbol) ||
        name == NULL) {
        return false;
    }
    dlerror();
    symbol = dlsym(library, name);
    if (symbol == NULL || dlerror() != NULL) {
        return false;
    }
    memcpy(target, &symbol, target_size);
    return true;
}
#endif

static void video_encoder_unload(VideoEncoder *encoder)
{
    if (encoder == NULL) {
        return;
    }
    if (encoder->jpeg_library_handle != NULL) {
        dlclose(encoder->jpeg_library_handle);
    }
    if (encoder->library_handle != NULL) {
        dlclose(encoder->library_handle);
    }
    encoder->jpeg_library_handle = NULL;
    encoder->library_handle = NULL;
}

#if defined(__arm__) || defined(__aarch64__)
static bool video_encoder_api_ready(const VideoEncoder *encoder)
{
    return encoder != NULL && encoder->api.create != NULL &&
           encoder->api.destroy != NULL && encoder->api.init != NULL &&
           encoder->api.uninit != NULL && encoder->api.alloc_input != NULL &&
           encoder->api.get_input != NULL && encoder->api.flush_input != NULL &&
           encoder->api.return_input != NULL &&
           encoder->api.release_input != NULL && encoder->api.add_input != NULL &&
           encoder->api.encode != NULL && encoder->api.used_input != NULL &&
           encoder->api.get_output != NULL && encoder->api.free_output != NULL &&
           encoder->api.set_parameter != NULL;
}

static bool video_encoder_copy_yuv420p(VideoEncoder *encoder,
                                       const VideoYuvFrame *frame,
                                       const CedarVencInputBuffer *input)
{
    uint32_t row;
    uint32_t chroma_width;
    uint32_t chroma_height;
    uint8_t *destination_u;
    uint8_t *destination_v;

    if (encoder == NULL || frame == NULL || frame->format !=
        VIDEO_YUV_FORMAT_YUV420P || frame->plane[0] == NULL ||
        frame->plane[1] == NULL || frame->plane[2] == NULL ||
        input == NULL || input->pAddrVirY == NULL ||
        input->pAddrVirC == NULL || frame->width != encoder->width ||
        frame->height != encoder->height || (frame->width & 1U) != 0U ||
        (frame->height & 1U) != 0U || frame->stride[0] < frame->width ||
        frame->stride[1] < frame->width / 2U ||
        frame->stride[2] < frame->width / 2U) {
        return false;
    }
    for (row = 0U; row < frame->height; ++row) {
        memcpy(input->pAddrVirY +
                   (size_t)row * encoder->stride,
               frame->plane[0] + (size_t)row * frame->stride[0],
               frame->width);
    }
    chroma_width = frame->width / 2U;
    chroma_height = frame->height / 2U;
    destination_u = input->pAddrVirC;
    destination_v = destination_u +
                    (size_t)encoder->stride * encoder->aligned_height / 4U;
    for (row = 0U; row < chroma_height; ++row) {
        memcpy(destination_u + (size_t)row * (encoder->stride / 2U),
               frame->plane[1] + (size_t)row * frame->stride[1],
               chroma_width);
        memcpy(destination_v + (size_t)row * (encoder->stride / 2U),
               frame->plane[2] + (size_t)row * frame->stride[2],
               chroma_width);
    }
    return true;
}

static bool video_encoder_copy_argb(VideoEncoder *encoder,
                                    const VideoFrame *frame,
                                    const CedarVencInputBuffer *input)
{
    uint32_t row;

    if (encoder == NULL || frame == NULL || input == NULL ||
        input->pAddrVirY == NULL || frame->pixels == NULL ||
        frame->width != encoder->width || frame->height != encoder->height ||
        frame->stride < frame->width * VIDEO_FRAME_CHANNELS) {
        return false;
    }
    for (row = 0U; row < frame->height; ++row) {
        memcpy(input->pAddrVirY + (size_t)row * encoder->stride,
               frame->pixels + (size_t)row * frame->stride,
               (size_t)frame->width * VIDEO_FRAME_CHANNELS);
    }
    return true;
}
#endif

static int video_encoder_init_format(VideoEncoder *encoder, uint32_t width,
                                     uint32_t height, uint32_t fps, int quality,
                                     size_t output_capacity,
                                     VideoEncoderInputFormat input_format)
{
#if !defined(__arm__) && !defined(__aarch64__)
    (void)encoder;
    (void)width;
    (void)height;
    (void)fps;
    (void)quality;
    (void)output_capacity;
    (void)input_format;
    errno = ENOSYS;
    return -1;
#else
    CedarVencBaseConfig config;
    CedarVencAllocateBufferParam buffer_param;
    CedarVencInputBuffer input_buffer;
    CedarVideoEncoder *handle;
    int aligned_width;
    int aligned_height;
    int quality_value = quality;

    if (encoder == NULL || width == 0U || height == 0U || fps == 0U ||
        output_capacity == 0U || width > UINT32_MAX - 15U ||
        height > UINT32_MAX - 15U) {
        errno = EINVAL;
        return -1;
    }
    memset(encoder, 0, sizeof(*encoder));
    encoder->library_handle = dlopen("libvencoder.so", RTLD_NOW | RTLD_LOCAL);
    if (encoder->library_handle == NULL) {
        errno = ENOSYS;
        goto fail;
    }
    /* Keep the JPEG plugin resident as long as the encoder is alive. */
    encoder->jpeg_library_handle =
        dlopen("libvenc_jpeg.so", RTLD_NOW | RTLD_LOCAL);
    if (encoder->jpeg_library_handle == NULL) {
        errno = ENOSYS;
        goto fail;
    }
#define VIDEO_ENCODER_LOAD(member, type, symbol)                             \
    do {                                                                       \
        if (!video_encoder_load_symbol(encoder->library_handle,              \
                                       &encoder->api.member,                  \
                                       sizeof(encoder->api.member),           \
                                       (symbol))) {                            \
            errno = ENOSYS;                                                    \
            goto fail;                                                          \
        }                                                                       \
    } while (0)
    VIDEO_ENCODER_LOAD(create, CedarVideoEncCreate, "VideoEncCreate");
    VIDEO_ENCODER_LOAD(destroy, CedarVideoEncDestroy, "VideoEncDestroy");
    VIDEO_ENCODER_LOAD(init, CedarVideoEncInit, "VideoEncInit");
    VIDEO_ENCODER_LOAD(uninit, CedarVideoEncUnInit, "VideoEncUnInit");
    VIDEO_ENCODER_LOAD(alloc_input, CedarAllocInputBuffer,
                       "AllocInputBuffer");
    VIDEO_ENCODER_LOAD(get_input, CedarGetOneAllocInputBuffer,
                       "GetOneAllocInputBuffer");
    VIDEO_ENCODER_LOAD(flush_input, CedarFlushCacheAllocInputBuffer,
                       "FlushCacheAllocInputBuffer");
    VIDEO_ENCODER_LOAD(return_input, CedarReturnOneAllocInputBuffer,
                       "ReturnOneAllocInputBuffer");
    VIDEO_ENCODER_LOAD(release_input, CedarReleaseAllocInputBuffer,
                       "ReleaseAllocInputBuffer");
    VIDEO_ENCODER_LOAD(add_input, CedarAddOneInputBuffer,
                       "AddOneInputBuffer");
    VIDEO_ENCODER_LOAD(encode, CedarVideoEncodeOneFrame,
                       "VideoEncodeOneFrame");
    VIDEO_ENCODER_LOAD(used_input, CedarAlreadyUsedInputBuffer,
                       "AlreadyUsedInputBuffer");
    VIDEO_ENCODER_LOAD(get_output, CedarGetOneBitstreamFrame,
                       "GetOneBitstreamFrame");
    VIDEO_ENCODER_LOAD(free_output, CedarFreeOneBitstreamFrame,
                       "FreeOneBitStreamFrame");
    VIDEO_ENCODER_LOAD(set_parameter, CedarVideoEncSetParameter,
                       "VideoEncSetParameter");
#undef VIDEO_ENCODER_LOAD
    if (!video_encoder_api_ready(encoder)) {
        errno = ENOSYS;
        goto fail;
    }
    aligned_width = (int)((width + 15U) & ~15U);
    aligned_height = (int)((height + 15U) & ~15U);
    encoder->width = width;
    encoder->height = height;
    encoder->input_format = input_format;
    encoder->stride = input_format == VIDEO_ENCODER_INPUT_ARGB8888 ?
                      width * VIDEO_FRAME_CHANNELS : (uint32_t)aligned_width;
    encoder->aligned_height = (uint32_t)aligned_height;
    encoder->output_capacity = output_capacity;
    encoder->output = malloc(output_capacity);
    if (encoder->output == NULL) {
        goto fail;
    }
    memset(&config, 0, sizeof(config));
    config.nInputWidth = width;
    config.nInputHeight = height;
    config.nDstWidth = width;
    config.nDstHeight = height;
    config.nStride = encoder->stride;
    config.eInputFormat = input_format == VIDEO_ENCODER_INPUT_ARGB8888 ?
                          CEDAR_VENC_PIXEL_ARGB : CEDAR_VENC_PIXEL_YUV420P;
    handle = encoder->api.create(CEDAR_VENC_CODEC_JPEG);
    if (handle == NULL) {
        errno = ENODEV;
        goto fail;
    }
    encoder->encoder = handle;
    if (encoder->api.set_parameter(handle, 0x200, &quality_value) != 0 ||
        encoder->api.init(handle, &config) != 0) {
        errno = EIO;
        goto fail;
    }
    /* Keep the hardware session marked active so VideoEncDestroy can perform
     * the vendor-side uninitialization if a later allocation step fails. */
    encoder->initialized = true;
    memset(&buffer_param, 0, sizeof(buffer_param));
    buffer_param.nBufferNum = 1U;
    if (input_format == VIDEO_ENCODER_INPUT_ARGB8888) {
        buffer_param.nSizeY = encoder->stride * (uint32_t)aligned_height;
        buffer_param.nSizeC = 0U;
    } else {
        buffer_param.nSizeY = (uint32_t)aligned_width *
                              (uint32_t)aligned_height;
        buffer_param.nSizeC = buffer_param.nSizeY / 2U;
    }
    encoder->input_size = (size_t)buffer_param.nSizeY + buffer_param.nSizeC;
    if (encoder->api.alloc_input(handle, &buffer_param) != 0) {
        errno = ENOMEM;
        goto fail;
    }
    encoder->input_buffers_allocated = true;
    memset(&input_buffer, 0, sizeof(input_buffer));
    if (encoder->api.get_input(handle, &input_buffer) != 0 ||
        input_buffer.pAddrVirY == NULL ||
        (input_format == VIDEO_ENCODER_INPUT_YUV420P &&
         input_buffer.pAddrVirC == NULL)) {
        errno = EIO;
        goto fail;
    }
    encoder->input_memory.pAddrVirY = input_buffer.pAddrVirY;
    encoder->input_memory.pAddrVirC = input_buffer.pAddrVirC;
    (void)encoder->api.return_input(handle, &input_buffer);
    encoder->available = true;
    LOG_INFO("Video encoder: Cedar hardware MJPEG %ux%u stride=%u "
             "format=%s quality=%d", width, height, encoder->stride,
             input_format == VIDEO_ENCODER_INPUT_ARGB8888 ? "ARGB8888" :
             "YUV420P", quality);
    return 0;

fail:
    video_encoder_close(encoder);
    return -1;
#endif
}

int video_encoder_init(VideoEncoder *encoder, uint32_t width,
                       uint32_t height, uint32_t fps, int quality,
                       size_t output_capacity)
{
    return video_encoder_init_format(encoder, width, height, fps, quality,
                                     output_capacity,
                                     VIDEO_ENCODER_INPUT_YUV420P);
}

int video_encoder_init_argb(VideoEncoder *encoder, uint32_t width,
                            uint32_t height, uint32_t fps, int quality,
                            size_t output_capacity)
{
    return video_encoder_init_format(encoder, width, height, fps, quality,
                                     output_capacity,
                                     VIDEO_ENCODER_INPUT_ARGB8888);
}

void video_encoder_close(VideoEncoder *encoder)
{
#if defined(__arm__) || defined(__aarch64__)
    CedarVideoEncoder *handle;

    if (encoder == NULL) {
        return;
    }
    handle = (CedarVideoEncoder *)encoder->encoder;
    if (handle != NULL && encoder->input_buffers_allocated &&
        encoder->api.release_input != NULL) {
        (void)encoder->api.release_input(handle);
    }
    if (handle != NULL && encoder->api.destroy != NULL) {
        encoder->api.destroy(handle);
    }
#else
    (void)encoder;
#endif
    if (encoder != NULL) {
        free(encoder->output);
        encoder->output = NULL;
        encoder->encoder = NULL;
        encoder->initialized = false;
        encoder->available = false;
        encoder->input_buffers_allocated = false;
        video_encoder_unload(encoder);
    }
}

bool video_encoder_is_available(const VideoEncoder *encoder)
{
    return encoder != NULL && encoder->initialized && encoder->available;
}

static bool video_encoder_encode_timed(VideoEncoder *encoder,
                                       const VideoYuvFrame *yuv_frame,
                                       const VideoFrame *argb_frame,
                                       const uint8_t **jpeg_data,
                                       size_t *jpeg_size,
                                       VideoEncoderTiming *timing)
{
#if !defined(__arm__) && !defined(__aarch64__)
    (void)encoder;
    (void)yuv_frame;
    (void)argb_frame;
    (void)jpeg_data;
    (void)jpeg_size;
    if (timing != NULL) {
        memset(timing, 0, sizeof(*timing));
    }
    return false;
#else
    CedarVideoEncoder *handle;
    CedarVencInputBuffer input_buffer;
    CedarVencOutputBuffer output_buffer;
    uint64_t total_start_us;
    uint64_t stage_start_us;
    bool input_added = false;
    bool output_acquired = false;
    bool encode_succeeded = false;
    bool success = false;

    if (timing != NULL) {
        memset(timing, 0, sizeof(*timing));
    }
    if (!video_encoder_is_available(encoder) || jpeg_data == NULL ||
        jpeg_size == NULL ||
        (encoder->input_format == VIDEO_ENCODER_INPUT_YUV420P &&
         yuv_frame == NULL) ||
        (encoder->input_format == VIDEO_ENCODER_INPUT_ARGB8888 &&
         argb_frame == NULL)) {
        return false;
    }
    total_start_us = video_encoder_now_us();
    handle = (CedarVideoEncoder *)encoder->encoder;
    memset(&input_buffer, 0, sizeof(input_buffer));
    stage_start_us = video_encoder_now_us();
    if (encoder->api.get_input(handle, &input_buffer) != 0 ||
        input_buffer.pAddrVirY == NULL ||
        (encoder->input_format == VIDEO_ENCODER_INPUT_YUV420P &&
         input_buffer.pAddrVirC == NULL)) {
        goto done;
    }
    if (timing != NULL) {
        timing->input_acquire_time_us =
            video_encoder_now_us() - stage_start_us;
    }
    stage_start_us = video_encoder_now_us();
    if ((encoder->input_format == VIDEO_ENCODER_INPUT_YUV420P &&
         !video_encoder_copy_yuv420p(encoder, yuv_frame, &input_buffer)) ||
        (encoder->input_format == VIDEO_ENCODER_INPUT_ARGB8888 &&
         !video_encoder_copy_argb(encoder, argb_frame, &input_buffer))) {
        (void)encoder->api.return_input(handle, &input_buffer);
        goto done;
    }
    if (timing != NULL) {
        timing->input_copy_time_us = video_encoder_now_us() - stage_start_us;
    }
    stage_start_us = video_encoder_now_us();
    input_buffer.nWidth = (int)encoder->width;
    input_buffer.nHeight = (int)encoder->height;
    input_buffer.nAlign = (int)encoder->stride;
    input_buffer.nPts = (int64_t)total_start_us;
    if (timing != NULL) {
        timing->input_prepare_time_us = video_encoder_now_us() - stage_start_us;
    }
    stage_start_us = video_encoder_now_us();
    if (encoder->api.flush_input(handle, &input_buffer) != 0) {
        (void)encoder->api.return_input(handle, &input_buffer);
        goto done;
    }
    if (timing != NULL) {
        timing->cache_flush_time_us = video_encoder_now_us() - stage_start_us;
        timing->handoff_time_us = timing->input_acquire_time_us +
                                  timing->input_copy_time_us +
                                  timing->cache_flush_time_us +
                                  timing->input_prepare_time_us;
    }
    stage_start_us = video_encoder_now_us();
    if (encoder->api.add_input(handle, &input_buffer) != 0) {
        (void)encoder->api.return_input(handle, &input_buffer);
        goto done;
    }
    input_added = true;
    if (timing != NULL) {
        timing->submit_time_us = video_encoder_now_us() - stage_start_us;
    }
    stage_start_us = video_encoder_now_us();
    if (encoder->api.encode(handle) < 0) {
        goto release_input;
    }
    encode_succeeded = true;
    if (timing != NULL) {
        timing->hardware_encode_time_us = video_encoder_now_us() - stage_start_us;
    }
release_input:
    (void)encoder->api.used_input(handle, &input_buffer);
    (void)encoder->api.return_input(handle, &input_buffer);
    input_added = false;
    if (!encode_succeeded) {
        goto done;
    }
    memset(&output_buffer, 0, sizeof(output_buffer));
    stage_start_us = video_encoder_now_us();
    if (encoder->api.get_output(handle, &output_buffer) != 0 ||
        output_buffer.pData0 == NULL || output_buffer.nSize0 == 0U) {
        goto done;
    }
    output_acquired = true;
    if ((uint64_t)output_buffer.nSize0 + output_buffer.nSize1 >
        encoder->output_capacity) {
        goto release_output;
    }
    memcpy(encoder->output, output_buffer.pData0, output_buffer.nSize0);
    if (output_buffer.nSize1 > 0U && output_buffer.pData1 == NULL) {
        goto release_output;
    }
    if (output_buffer.nSize1 > 0U && output_buffer.pData1 != NULL) {
        memcpy(encoder->output + output_buffer.nSize0, output_buffer.pData1,
               output_buffer.nSize1);
    }
    if (timing != NULL) {
        timing->acquire_time_us = video_encoder_now_us() - stage_start_us;
    }
    *jpeg_data = encoder->output;
    *jpeg_size = (size_t)output_buffer.nSize0 + output_buffer.nSize1;
    if (timing != NULL) {
        timing->output_size = *jpeg_size;
        timing->encoded = true;
    }
    success = true;
release_output:
    if (output_acquired) {
        uint64_t release_start_us = video_encoder_now_us();

        (void)encoder->api.free_output(handle, &output_buffer);
        if (timing != NULL) {
            timing->release_time_us = video_encoder_now_us() - release_start_us;
        }
    }
done:
    if (input_added) {
        (void)encoder->api.used_input(handle, &input_buffer);
        (void)encoder->api.return_input(handle, &input_buffer);
    }
    if (timing != NULL) {
        timing->total_time_us = video_encoder_now_us() - total_start_us;
    }
    if (!success) {
        encoder->available = false;
    }
    return success;
#endif
}

bool video_encoder_encode_yuv_timed(VideoEncoder *encoder,
                                    const VideoYuvFrame *frame,
                                    const uint8_t **jpeg_data,
                                    size_t *jpeg_size,
                                    VideoEncoderTiming *timing)
{
    return video_encoder_encode_timed(encoder, frame, NULL, jpeg_data,
                                      jpeg_size, timing);
}

bool video_encoder_encode_yuv(VideoEncoder *encoder, const VideoYuvFrame *frame,
                              const uint8_t **jpeg_data, size_t *jpeg_size)
{
    return video_encoder_encode_yuv_timed(encoder, frame, jpeg_data, jpeg_size,
                                          NULL);
}

bool video_encoder_encode_argb_timed(VideoEncoder *encoder,
                                     const VideoFrame *frame,
                                     const uint8_t **jpeg_data,
                                     size_t *jpeg_size,
                                     VideoEncoderTiming *timing)
{
    return video_encoder_encode_timed(encoder, NULL, frame, jpeg_data,
                                      jpeg_size, timing);
}
