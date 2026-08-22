#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "jpeg_codec.h"
#include "cedar_vdecoder_abi.h"
#include "console_log.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#define JPEG_CODEC_VERSION JPEG_LIB_VERSION

typedef void *(*JpegHardwareCreate)(void);
typedef void (*JpegHardwareDestroy)(void *decoder);
typedef void (*JpegHardwareSetDataSourceBuf)(void *decoder, char *buffer,
                                             int buffer_length,
                                             int scale_ratio,
                                             int output_type);
typedef JpegHardwareFrame *(*JpegHardwareGetFrame)(void *decoder);
typedef void (*JpegHardwareDestroyVideoDecoder)(void *decoder);

typedef struct {
    void *video_decoder;
    JpegHardwareFrame image;
    char *url;
    char *source_buffer;
    int source_length;
    int scale_down_enabled;
    int horizontal_scale_down_ratio;
    int vertical_scale_down_ratio;
    int decoded_pixel_format;
    int output_data_type;
    void *memory_operations;
} JpegHardwareContext;

typedef struct {
    JpegHardwareCreate create;
    JpegHardwareDestroy destroy;
    JpegHardwareSetDataSourceBuf set_data_source_buf;
    JpegHardwareGetFrame get_frame;
} JpegHardwareApi;

static void *jpeg_hardware_vdecoder_library;
#if defined(__arm__) || defined(__aarch64__)
static void *jpeg_native_vdecoder_library;
#endif

typedef struct {
    void *library;
    void *jpeg_wrapper_library;
    void *jpeg_wrapper_decoder;
    JpegHardwareCreate jpeg_wrapper_create;
    JpegHardwareDestroy jpeg_wrapper_destroy;
    CedarCreateVideoDecoder create;
    CedarDestroyVideoDecoder destroy;
    CedarInitializeVideoDecoder initialize;
    CedarResetVideoDecoder reset;
    CedarDecodeVideoStream decode;
    CedarRequestVideoStreamBuffer request_stream_buffer;
    CedarSubmitVideoStreamData submit_stream_data;
    CedarRequestPicture request_picture;
    CedarReturnPicture return_picture;
    CedarVideoDecoder *decoder;
    uint32_t width;
    uint32_t height;
    uint64_t frame_number;
    CedarVideoPicture *held_picture;
} JpegNativeContext;

static uint64_t jpeg_codec_now_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return 0U;
    }
    return (uint64_t)now.tv_sec * 1000000ULL +
           (uint64_t)now.tv_nsec / 1000ULL;
}

static void jpeg_codec_error_exit(j_common_ptr common)
{
    JpegCodecError *error = (JpegCodecError *)common->client_data;

    if (error != NULL) {
        error->message[0] = '\0';
        if (common->err != NULL && common->err->format_message != NULL) {
            common->err->format_message(common, error->message);
        }
        longjmp(error->jump, 1);
    }
    abort();
}

static void jpeg_codec_unload(JpegCodecApi *api)
{
    if (api == NULL) {
        return;
    }
    if (api->library_handle != NULL) {
        dlclose(api->library_handle);
    }
    memset(api, 0, sizeof(*api));
}

static bool jpeg_codec_load_symbol(void *library, void **target,
                                   const char *name)
{
    void *symbol;

    if (library == NULL || target == NULL || name == NULL) {
        return false;
    }
    dlerror();
    symbol = dlsym(library, name);
    if (symbol == NULL || dlerror() != NULL) {
        return false;
    }
    memcpy(target, &symbol, sizeof(symbol));
    return true;
}

static bool jpeg_hardware_load_symbol(void *library, void *target,
                                      size_t target_size, const char *name)
{
    void *symbol;

    if (target == NULL || target_size != sizeof(symbol) ||
        !jpeg_codec_load_symbol(library, &symbol, name)) {
        return false;
    }
    memcpy(target, &symbol, target_size);
    return true;
}

static void jpeg_native_context_close(JpegNativeContext *context)
{
    if (context == NULL) {
        return;
    }
    if (context->held_picture != NULL && context->return_picture != NULL) {
        (void)context->return_picture(context->decoder,
                                       context->held_picture);
        context->held_picture = NULL;
    }
    if (context->destroy != NULL && context->decoder != NULL) {
        context->destroy(context->decoder);
    }
    if (context->jpeg_wrapper_destroy != NULL &&
        context->jpeg_wrapper_decoder != NULL) {
        context->jpeg_wrapper_destroy(context->jpeg_wrapper_decoder);
    }
    /* Keep the wrapper library resident.  Its plugin registry is global to
     * the Cedar process and the board's vendor allocator is not safe to
     * unload after registration. */
    free(context);
}

static bool jpeg_native_decoder_init(JpegDecoder *decoder, uint32_t width)
{
#if !defined(__arm__) && !defined(__aarch64__)
    (void)decoder;
    (void)width;
    return false;
#else
    JpegNativeContext *context;
    CedarVideoStreamInfo stream_info;
    CedarVConfig config;

    if (decoder == NULL || width == 0U) {
        return false;
    }
    if (jpeg_native_vdecoder_library == NULL) {
        jpeg_native_vdecoder_library =
            dlopen("libvdecoder.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (jpeg_native_vdecoder_library == NULL) {
        return false;
    }
    context = calloc(1U, sizeof(*context));
    if (context == NULL) {
        return false;
    }
    context->library = jpeg_native_vdecoder_library;
    context->jpeg_wrapper_library =
        dlopen("libjpegdecode.so", RTLD_NOW | RTLD_LOCAL);
    if (context->jpeg_wrapper_library == NULL ||
        !jpeg_hardware_load_symbol(context->jpeg_wrapper_library,
                                   &context->jpeg_wrapper_create,
                                   sizeof(context->jpeg_wrapper_create),
                                   "JpegDecoderCreate") ||
        !jpeg_hardware_load_symbol(context->jpeg_wrapper_library,
                                   &context->jpeg_wrapper_destroy,
                                   sizeof(context->jpeg_wrapper_destroy),
                                   "JpegDecoderDestory")) {
        jpeg_native_context_close(context);
        return false;
    }
    /* JpegDecoderCreate registers libawmjpeg.so with libvdecoder.  The
     * direct vdecoder API does not perform that registration itself. */
    context->jpeg_wrapper_decoder = context->jpeg_wrapper_create();
    if (context->jpeg_wrapper_decoder == NULL) {
        jpeg_native_context_close(context);
        return false;
    }
    if (!jpeg_hardware_load_symbol(context->library, &context->create,
                                   sizeof(context->create),
                                   "CreateVideoDecoder") ||
        !jpeg_hardware_load_symbol(context->library, &context->destroy,
                                   sizeof(context->destroy),
                                   "DestroyVideoDecoder") ||
        !jpeg_hardware_load_symbol(context->library, &context->initialize,
                                   sizeof(context->initialize),
                                   "InitializeVideoDecoder") ||
        !jpeg_hardware_load_symbol(context->library, &context->reset,
                                   sizeof(context->reset),
                                   "ResetVideoDecoder") ||
        !jpeg_hardware_load_symbol(context->library, &context->decode,
                                   sizeof(context->decode),
                                   "DecodeVideoStream") ||
        !jpeg_hardware_load_symbol(context->library,
                                   &context->request_stream_buffer,
                                   sizeof(context->request_stream_buffer),
                                   "RequestVideoStreamBuffer") ||
        !jpeg_hardware_load_symbol(context->library,
                                   &context->submit_stream_data,
                                   sizeof(context->submit_stream_data),
                                   "SubmitVideoStreamData") ||
        !jpeg_hardware_load_symbol(context->library, &context->request_picture,
                                   sizeof(context->request_picture),
                                   "RequestPicture") ||
        !jpeg_hardware_load_symbol(context->library, &context->return_picture,
                                   sizeof(context->return_picture),
                                   "ReturnPicture")) {
        jpeg_native_context_close(context);
        return false;
    }
    context->decoder = context->create();
    if (context->decoder == NULL) {
        jpeg_native_context_close(context);
        return false;
    }
    memset(&stream_info, 0, sizeof(stream_info));
    stream_info.eCodecFormat = CEDAR_VIDEO_CODEC_FORMAT_MJPEG;
    stream_info.nWidth = (int)width;
    stream_info.nHeight = 720;
    stream_info.nFrameRate = 30;
    stream_info.nFrameDuration = 33333;
    memset(&config, 0, sizeof(config));
    config.value[CEDAR_VCONFIG_OUTPUT_PIXEL_FORMAT] =
        CEDAR_PIXEL_FORMAT_YUV_PLANER_420;
    if (context->initialize(context->decoder, &stream_info, &config) != 0) {
        jpeg_native_context_close(context);
        return false;
    }
    context->width = width;
    context->height = 720U;
    decoder->native_context = context;
    decoder->native_initialized = true;
    return true;
#endif
}

static void jpeg_native_decoder_close(JpegDecoder *decoder)
{
    JpegNativeContext *context;

    if (decoder == NULL || !decoder->native_initialized) {
        return;
    }
    context = (JpegNativeContext *)decoder->native_context;
    jpeg_native_context_close(context);
    decoder->native_context = NULL;
    decoder->native_initialized = false;
}

static JpegDecodedPixelFormat jpeg_native_pixel_format(int format)
{
    switch (format) {
    case CEDAR_PIXEL_FORMAT_YUV_PLANER_420:
        return JPEG_DECODED_FORMAT_YUV420P;
    case CEDAR_PIXEL_FORMAT_YV12:
        return JPEG_DECODED_FORMAT_YV12;
    case CEDAR_PIXEL_FORMAT_NV21:
        return JPEG_DECODED_FORMAT_NV21;
    case CEDAR_PIXEL_FORMAT_NV12:
        return JPEG_DECODED_FORMAT_NV12;
    default:
        return JPEG_DECODED_FORMAT_UNKNOWN;
    }
}

static bool jpeg_native_decode(JpegDecoder *decoder, const uint8_t *jpeg_data,
                               size_t jpeg_size, JpegDecodedFrame *frame,
                               JpegDecoderTiming *timing)
{
    JpegNativeContext *context;
    CedarVideoStreamDataInfo data;
    CedarVideoPicture *picture;
    char *buffer;
    char *ring_buffer;
    int buffer_size;
    int ring_buffer_size;
    int decode_result;
    unsigned int attempt;

    if (decoder == NULL || !decoder->native_initialized ||
        jpeg_data == NULL || jpeg_size == 0U || frame == NULL ||
        jpeg_size > INT_MAX) {
        return false;
    }
    context = (JpegNativeContext *)decoder->native_context;
    if (context == NULL || context->decoder == NULL) {
        return false;
    }
    memset(frame, 0, sizeof(*frame));
    buffer = NULL;
    ring_buffer = NULL;
    buffer_size = 0;
    ring_buffer_size = 0;
    {
        uint64_t start_us = jpeg_codec_now_us();
        int result = context->request_stream_buffer(
            context->decoder, (int)jpeg_size, &buffer, &buffer_size,
            &ring_buffer, &ring_buffer_size, 0);

        if (timing != NULL) {
            timing->decoder_buffer_request_time_us +=
                jpeg_codec_now_us() - start_us;
        }
        if (result != 0 || buffer == NULL || buffer_size < 0 ||
            ring_buffer_size < 0 || (size_t)buffer_size +
            (size_t)ring_buffer_size < jpeg_size) {
            if (context->reset != NULL) {
                context->reset(context->decoder);
            }
            return false;
        }
    }
    {
        uint64_t start_us = jpeg_codec_now_us();
        size_t first_size = jpeg_size < (size_t)buffer_size ?
                            jpeg_size : (size_t)buffer_size;

        memcpy(buffer, jpeg_data, first_size);
        if (jpeg_size > first_size) {
            memcpy(ring_buffer, jpeg_data + first_size,
                   jpeg_size - first_size);
        }
        if (timing != NULL) {
            timing->input_copy_time_us += jpeg_codec_now_us() - start_us;
        }
    }
    memset(&data, 0, sizeof(data));
    data.pData = buffer;
    data.nLength = (int)jpeg_size;
    data.nPts = (int64_t)context->frame_number * 33333LL;
    data.nPcr = data.nPts;
    data.bIsFirstPart = 1;
    data.bIsLastPart = 1;
    data.nID = (int)(context->frame_number & 0x7fffffffU);
    data.nStreamIndex = 0;
    data.bValid = 1;
    context->frame_number++;
    {
        uint64_t start_us = jpeg_codec_now_us();

        if (context->submit_stream_data(context->decoder, &data, 0) != 0) {
            if (timing != NULL) {
                timing->decoder_submit_time_us +=
                    jpeg_codec_now_us() - start_us;
            }
            if (context->reset != NULL) {
                context->reset(context->decoder);
            }
            return false;
        }
        if (timing != NULL) {
            timing->decoder_submit_time_us +=
                jpeg_codec_now_us() - start_us;
        }
    }
    decode_result = CEDAR_VDECODE_RESULT_OK;
    for (attempt = 0U; attempt < 4U; ++attempt) {
        uint64_t start_us = jpeg_codec_now_us();

        decode_result = context->decode(context->decoder, 0, 0, 0, data.nPts);
        if (timing != NULL) {
            timing->decoder_decode_time_us += jpeg_codec_now_us() - start_us;
        }
        if (decode_result == CEDAR_VDECODE_RESULT_FRAME_DECODED ||
            decode_result == CEDAR_VDECODE_RESULT_KEYFRAME_DECODED) {
            break;
        }
        if (decode_result != CEDAR_VDECODE_RESULT_OK &&
            decode_result != CEDAR_VDECODE_RESULT_CONTINUE) {
            break;
        }
    }
    if (decode_result != CEDAR_VDECODE_RESULT_FRAME_DECODED &&
        decode_result != CEDAR_VDECODE_RESULT_KEYFRAME_DECODED) {
        if (context->reset != NULL) {
            context->reset(context->decoder);
        }
        return false;
    }
    {
        uint64_t start_us = jpeg_codec_now_us();

        picture = context->request_picture(context->decoder, 0);
        if (timing != NULL) {
            timing->decoder_request_picture_time_us +=
                jpeg_codec_now_us() - start_us;
        }
    }
    if (picture == NULL || picture->pData0 == NULL || picture->nWidth <= 0 ||
        picture->nHeight <= 0 || picture->nLineStride <= 0) {
        if (context->reset != NULL) {
            context->reset(context->decoder);
        }
        return false;
    }
    frame->format = jpeg_native_pixel_format(picture->ePixelFormat);
    frame->width = (uint32_t)picture->nWidth;
    frame->height = (uint32_t)picture->nHeight;
    frame->stride[0] = (uint32_t)picture->nLineStride;
    frame->stride[1] = frame->format == JPEG_DECODED_FORMAT_NV12 ||
                       frame->format == JPEG_DECODED_FORMAT_NV21 ?
                       frame->stride[0] : frame->stride[0] / 2U;
    frame->stride[2] = frame->stride[1];
    frame->left_offset = picture->nLeftOffset > 0 ?
                         (uint32_t)picture->nLeftOffset : 0U;
    frame->top_offset = picture->nTopOffset > 0 ?
                        (uint32_t)picture->nTopOffset : 0U;
    frame->plane[0] = (uint8_t *)picture->pData0;
    frame->plane[1] = (uint8_t *)picture->pData1;
    frame->plane[2] = (uint8_t *)picture->pData2;
    frame->opaque = picture;
    context->held_picture = picture;
    if (frame->format == JPEG_DECODED_FORMAT_UNKNOWN) {
        (void)context->return_picture(context->decoder, picture);
        context->held_picture = NULL;
        memset(frame, 0, sizeof(*frame));
        return false;
    }
    return true;
}

static bool jpeg_native_return_frame(JpegDecoder *decoder,
                                     JpegDecodedFrame *frame,
                                     JpegDecoderTiming *timing)
{
    JpegNativeContext *context;
    int result;

    if (decoder == NULL || frame == NULL || frame->opaque == NULL ||
        !decoder->native_initialized) {
        return false;
    }
    context = (JpegNativeContext *)decoder->native_context;
    if (context == NULL || context->return_picture == NULL) {
        return false;
    }
    {
        uint64_t start_us = jpeg_codec_now_us();

        result = context->return_picture(context->decoder,
                                          (CedarVideoPicture *)frame->opaque);
        if (timing != NULL) {
            timing->decoder_return_picture_time_us +=
                jpeg_codec_now_us() - start_us;
        }
    }
    if (result != 0) {
        return false;
    }
    context->held_picture = NULL;
    memset(frame, 0, sizeof(*frame));
    return true;
}

static uint8_t jpeg_clamp_u8(int value)
{
    if (value < 0) {
        return 0U;
    }
    if (value > 255) {
        return 255U;
    }
    return (uint8_t)value;
}

bool jpeg_decoded_frame_to_rgba(const JpegDecodedFrame *source,
                                VideoFrame *destination)
{
    uint32_t row;

    if (source == NULL || destination == NULL || destination->pixels == NULL ||
        source->plane[0] == NULL || source->width < destination->width +
        source->left_offset || source->height < destination->height +
        source->top_offset || source->stride[0] == 0U) {
        return false;
    }
    if ((source->format == JPEG_DECODED_FORMAT_YUV420P ||
         source->format == JPEG_DECODED_FORMAT_YV12) &&
        (source->plane[1] == NULL || source->plane[2] == NULL)) {
        return false;
    }
    if ((source->format == JPEG_DECODED_FORMAT_NV12 ||
         source->format == JPEG_DECODED_FORMAT_NV21) &&
        source->plane[1] == NULL) {
        return false;
    }
    for (row = 0U; row < destination->height; ++row) {
        uint32_t column;
        uint32_t source_y = row + source->top_offset;
        const uint8_t *y_plane = source->plane[0] +
            (size_t)source_y * source->stride[0] + source->left_offset;
        uint8_t *output = destination->pixels +
            (size_t)row * destination->stride;

        for (column = 0U; column < destination->width; ++column) {
            uint32_t source_x = column + source->left_offset;
            uint32_t chroma_x = source_x / 2U;
            uint32_t chroma_y = source_y / 2U;
            int y = y_plane[column];
            int u;
            int v;
            int c;
            uint8_t *pixel = output + (size_t)column * VIDEO_FRAME_CHANNELS;

            if (source->format == JPEG_DECODED_FORMAT_YUV420P ||
                source->format == JPEG_DECODED_FORMAT_YV12) {
                const uint8_t *plane1 = source->plane[1] +
                    (size_t)chroma_y * source->stride[1] + chroma_x;
                const uint8_t *plane2 = source->plane[2] +
                    (size_t)chroma_y * source->stride[2] + chroma_x;

                if (source->format == JPEG_DECODED_FORMAT_YV12) {
                    v = *plane1;
                    u = *plane2;
                } else {
                    u = *plane1;
                    v = *plane2;
                }
            } else {
                const uint8_t *chroma = source->plane[1] +
                    (size_t)chroma_y * source->stride[1] + chroma_x * 2U;

                if (source->format == JPEG_DECODED_FORMAT_NV21) {
                    v = chroma[0];
                    u = chroma[1];
                } else {
                    u = chroma[0];
                    v = chroma[1];
                }
            }
            c = y - 16;
            if (c < 0) {
                c = 0;
            }
            pixel[0] = jpeg_clamp_u8((298 * c + 409 * (v - 128) + 128) >> 8);
            pixel[1] = jpeg_clamp_u8((298 * c - 100 * (u - 128) -
                                       208 * (v - 128) + 128) >> 8);
            pixel[2] = jpeg_clamp_u8((298 * c + 516 * (u - 128) + 128) >> 8);
            pixel[3] = 255U;
        }
    }
    return true;
}

bool jpeg_decoder_is_native(const JpegDecoder *decoder)
{
    return decoder != NULL && decoder->initialized &&
           decoder->native_initialized;
}

bool jpeg_decoder_decode_native_timed(JpegDecoder *decoder,
                                      const uint8_t *jpeg_data,
                                      size_t jpeg_size,
                                      JpegDecodedFrame *frame,
                                      JpegDecoderTiming *timing)
{
    if (timing != NULL) {
        memset(timing, 0, sizeof(*timing));
    }
    if (!jpeg_decoder_is_native(decoder) || frame == NULL) {
        return false;
    }
    return jpeg_native_decode(decoder, jpeg_data, jpeg_size, frame, timing);
}

bool jpeg_decoder_return_native_frame(JpegDecoder *decoder,
                                      JpegDecodedFrame *frame,
                                      JpegDecoderTiming *timing)
{
    return jpeg_native_return_frame(decoder, frame, timing);
}

static bool jpeg_hardware_decoder_init(JpegDecoder *decoder)
{
    JpegHardwareApi hardware;
    void *library;
    void *object;

    if (decoder == NULL) {
        return false;
    }
    library = dlopen("libjpegdecode.so", RTLD_NOW | RTLD_LOCAL);
    if (library == NULL) {
        return false;
    }
    memset(&hardware, 0, sizeof(hardware));
    if (!jpeg_hardware_load_symbol(library, &hardware.create,
                                   sizeof(hardware.create),
                                   "JpegDecoderCreate") ||
        !jpeg_hardware_load_symbol(library, &hardware.destroy,
                                   sizeof(hardware.destroy),
                                   "JpegDecoderDestory") ||
        !jpeg_hardware_load_symbol(library, &hardware.set_data_source_buf,
                                   sizeof(hardware.set_data_source_buf),
                                   "JpegDecoderSetDataSourceBuf") ||
        !jpeg_hardware_load_symbol(library, &hardware.get_frame,
                                   sizeof(hardware.get_frame),
                                   "JpegDecoderGetFrame")) {
        dlclose(library);
        return false;
    }
    object = hardware.create();
    if (object == NULL) {
        dlclose(library);
        return false;
    }
    decoder->hardware_library = library;
    decoder->hardware_decoder = object;
    decoder->hardware_frame.rotation = 0;
    decoder->hardware_initialized = true;
    return true;
}

static void jpeg_hardware_decoder_close(JpegDecoder *decoder)
{
    JpegHardwareDestroy destroy = NULL;
    bool input_transferred;

    if (decoder == NULL) {
        return;
    }
    input_transferred = decoder->hardware_input_transferred;
    if (decoder->hardware_library != NULL) {
        (void)jpeg_hardware_load_symbol(decoder->hardware_library, &destroy,
                                        sizeof(destroy),
                                        "JpegDecoderDestory");
    }
    if (destroy != NULL && decoder->hardware_decoder != NULL) {
        destroy(decoder->hardware_decoder);
    }
    /* The vendor decoder registers global Cedar plugins.  Unloading its
     * shared object after destroy corrupts the vendor allocator on T113;
     * keep this tiny library resident for the process lifetime. */
    decoder->hardware_library = NULL;
    decoder->hardware_decoder = NULL;
    if (!input_transferred) {
        free(decoder->hardware_input);
    }
    decoder->hardware_input = NULL;
    decoder->hardware_input_transferred = false;
    decoder->hardware_initialized = false;
}

static bool jpeg_hardware_decoder_create_object(JpegDecoder *decoder)
{
    JpegHardwareCreate create = NULL;

    if (decoder == NULL || decoder->hardware_library == NULL) {
        return false;
    }
    if (!jpeg_hardware_load_symbol(decoder->hardware_library, &create,
                                   sizeof(create), "JpegDecoderCreate")) {
        return false;
    }
    decoder->hardware_decoder = create();
    return decoder->hardware_decoder != NULL;
}

static void jpeg_hardware_release_video_decoder(JpegDecoder *decoder)
{
    JpegHardwareContext *context;
    JpegHardwareDestroyVideoDecoder destroy = NULL;

    if (decoder == NULL || decoder->hardware_decoder == NULL) {
        return;
    }
    context = (JpegHardwareContext *)decoder->hardware_decoder;
    if (context->video_decoder == NULL) {
        return;
    }
    if (jpeg_hardware_vdecoder_library == NULL) {
        jpeg_hardware_vdecoder_library =
            dlopen("libvdecoder.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!jpeg_hardware_load_symbol(jpeg_hardware_vdecoder_library, &destroy,
                                   sizeof(destroy),
                                   "DestroyVideoDecoder")) {
        return;
    }
    destroy(context->video_decoder);
    context->video_decoder = NULL;
}

static int jpeg_codec_load(JpegCodecApi *api)
{
#if defined(__arm__) || defined(__aarch64__)
    /* The T113 image provides libjpeg.so.9, whose ABI is intentionally not
     * mixed with the development machine's libjpeg-turbo.so.8.  T113 uses
     * libjpegdecode.so for camera JPEG input; this path is only a safe
     * software fallback when an ABI-matched library is installed. */
    static const char *const names[] = {
        "libjpeg.so.9", NULL
    };
#else
    static const char *const names[] = {
        "libjpeg.so.8", "libjpeg.so", "libjpeg-turbo.so.2", NULL
    };
#endif
    size_t index;
    void *library = NULL;

    if (api == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(api, 0, sizeof(*api));
    for (index = 0U; names[index] != NULL; ++index) {
        library = dlopen(names[index], RTLD_LAZY | RTLD_LOCAL);
        if (library != NULL) {
            break;
        }
    }
    if (library == NULL) {
        errno = ENOSYS;
        return -1;
    }
    api->library_handle = library;
#define JPEG_LOAD(member, symbol)                                             \
    do {                                                                       \
        if (!jpeg_codec_load_symbol(library, (void **)&api->member, symbol)) { \
            jpeg_codec_unload(api);                                           \
            errno = ENOSYS;                                                    \
            return -1;                                                         \
        }                                                                      \
    } while (0)
    JPEG_LOAD(std_error, "jpeg_std_error");
    /* libjpeg 9 exports jpeg_version; older libjpeg builds do not. */
    (void)jpeg_codec_load_symbol(library, (void **)&api->version,
                                 "jpeg_version");
    api->library_version = JPEG_CODEC_VERSION;
    if (api->version != NULL && api->version() > 0) {
        api->library_version = api->version();
    }
    JPEG_LOAD(create_decompress, "jpeg_CreateDecompress");
    JPEG_LOAD(destroy_decompress, "jpeg_destroy_decompress");
    JPEG_LOAD(mem_src, "jpeg_mem_src");
    JPEG_LOAD(read_header, "jpeg_read_header");
    JPEG_LOAD(start_decompress, "jpeg_start_decompress");
    JPEG_LOAD(read_scanlines, "jpeg_read_scanlines");
    JPEG_LOAD(finish_decompress, "jpeg_finish_decompress");
    JPEG_LOAD(abort_decompress, "jpeg_abort_decompress");
    JPEG_LOAD(create_compress, "jpeg_CreateCompress");
    JPEG_LOAD(destroy_compress, "jpeg_destroy_compress");
    JPEG_LOAD(set_defaults, "jpeg_set_defaults");
    JPEG_LOAD(set_quality, "jpeg_set_quality");
    JPEG_LOAD(start_compress, "jpeg_start_compress");
    JPEG_LOAD(write_scanlines, "jpeg_write_scanlines");
    JPEG_LOAD(finish_compress, "jpeg_finish_compress");
    JPEG_LOAD(abort_compress, "jpeg_abort_compress");
#undef JPEG_LOAD
    return 0;
}

static bool jpeg_codec_prepare_error(JpegCodecApi *api,
                                     JpegCodecError *error,
                                     j_common_ptr common)
{
    if (api == NULL || error == NULL || common == NULL) {
        return false;
    }
    memset(error, 0, sizeof(*error));
    api->std_error(&error->public_error);
    error->public_error.error_exit = jpeg_codec_error_exit;
    common->err = &error->public_error;
    common->client_data = error;
    return true;
}

int jpeg_decoder_init(JpegDecoder *decoder, uint32_t width)
{
    if (decoder == NULL || width == 0U) {
        errno = EINVAL;
        return -1;
    }
    memset(decoder, 0, sizeof(*decoder));
    if ((uint64_t)width * 3U > SIZE_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    decoder->row_rgb = malloc((size_t)width * 3U);
    if (decoder->row_rgb == NULL) {
        return -1;
    }
    decoder->row_capacity = (size_t)width * 3U;
    if (jpeg_native_decoder_init(decoder, width)) {
        decoder->initialized = true;
        return 0;
    }
    if (jpeg_hardware_decoder_init(decoder)) {
        decoder->initialized = true;
        return 0;
    }
    if (jpeg_codec_load(&decoder->api) < 0) {
        free(decoder->row_rgb);
        memset(decoder, 0, sizeof(*decoder));
        LOG_WARN("JPEG decoder unavailable; libjpeg runtime was not found");
        return -1;
    }
    if (!jpeg_codec_prepare_error(&decoder->api, &decoder->error,
                                  (j_common_ptr)&decoder->cinfo)) {
        jpeg_codec_unload(&decoder->api);
        free(decoder->row_rgb);
        memset(decoder, 0, sizeof(*decoder));
        errno = EINVAL;
        return -1;
    }
    decoder->api.create_decompress(&decoder->cinfo,
                                   decoder->api.library_version,
                                   sizeof(decoder->cinfo));
    decoder->initialized = true;
    return 0;
}

void jpeg_decoder_close(JpegDecoder *decoder)
{
    if (decoder == NULL) {
        return;
    }
    if (decoder->native_initialized) {
        jpeg_native_decoder_close(decoder);
    } else if (decoder->hardware_initialized) {
        jpeg_hardware_decoder_close(decoder);
    } else if (decoder->initialized) {
        decoder->api.destroy_decompress(&decoder->cinfo);
    }
    jpeg_codec_unload(&decoder->api);
    free(decoder->row_rgb);
    memset(decoder, 0, sizeof(*decoder));
}

bool jpeg_decoder_decode_timed(JpegDecoder *decoder, const uint8_t *jpeg_data,
                               size_t jpeg_size, VideoFrame *destination,
                               JpegDecoderTiming *timing)
{
    uint32_t row;

    if (timing != NULL) {
        memset(timing, 0, sizeof(*timing));
    }

    if (decoder == NULL || !decoder->initialized || jpeg_data == NULL ||
        jpeg_size == 0U || destination == NULL || destination->pixels == NULL) {
        return false;
    }
    if (jpeg_size > ULONG_MAX) {
        return false;
    }
    if (decoder->native_initialized) {
        JpegDecodedFrame source;
        bool converted;
        bool returned;
        uint64_t convert_start_us;

        if (!jpeg_native_decode(decoder, jpeg_data, jpeg_size, &source,
                                timing)) {
            return false;
        }
        if (!decoder->native_frame_logged) {
            LOG_INFO("JPEG native picture: format=%d size=%ux%u stride=%u "
                     "offset=%u,%u",
                     (int)source.format, source.width, source.height,
                     source.stride[0], source.left_offset, source.top_offset);
            decoder->native_frame_logged = true;
        }
        convert_start_us = jpeg_codec_now_us();
        converted = jpeg_decoded_frame_to_rgba(&source, destination);
        if (timing != NULL) {
            timing->format_convert_time_us +=
                jpeg_codec_now_us() - convert_start_us;
        }
        returned = jpeg_native_return_frame(decoder, &source, timing);
        return converted && returned;
    }
    if (decoder->hardware_initialized) {
        JpegHardwareApi hardware;
        JpegHardwareFrame *source;
        uint32_t row;

        if (decoder->hardware_decoder == NULL &&
            !jpeg_hardware_decoder_create_object(decoder)) {
            return false;
        }
        memset(&hardware, 0, sizeof(hardware));
        if (!jpeg_hardware_load_symbol(decoder->hardware_library,
                                       &hardware.set_data_source_buf,
                                       sizeof(hardware.set_data_source_buf),
                                       "JpegDecoderSetDataSourceBuf") ||
            !jpeg_hardware_load_symbol(decoder->hardware_library,
                                       &hardware.get_frame,
                                       sizeof(hardware.get_frame),
                                       "JpegDecoderGetFrame")) {
            return false;
        }
        if (jpeg_size > INT_MAX) {
            return false;
        }
        {
            uint64_t copy_start_us = jpeg_codec_now_us();

            decoder->hardware_input = malloc(jpeg_size);
            if (decoder->hardware_input == NULL) {
                return false;
            }
            memcpy(decoder->hardware_input, jpeg_data, jpeg_size);
            if (timing != NULL) {
                timing->input_copy_time_us +=
                    jpeg_codec_now_us() - copy_start_us;
            }
        }
        decoder->hardware_input_transferred = true;
        {
            uint64_t submit_start_us = jpeg_codec_now_us();

            hardware.set_data_source_buf(decoder->hardware_decoder,
                                         (char *)decoder->hardware_input,
                                         (int)jpeg_size, 0, 5);
            if (timing != NULL) {
                timing->decoder_submit_time_us +=
                    jpeg_codec_now_us() - submit_start_us;
            }
        }
        {
            uint64_t get_frame_start_us = jpeg_codec_now_us();

            source = hardware.get_frame(decoder->hardware_decoder);
            if (timing != NULL) {
                /* The vendor wrapper performs decode and picture acquire in
                 * one GetFrame call, so they cannot be split at this ABI. */
                timing->decoder_get_frame_time_us +=
                    jpeg_codec_now_us() - get_frame_start_us;
            }
        }
        /* The vendor implementation frees mSrcBuf before returning from
         * JpegDecoderGetFrame().  Do not retain or free this pointer again. */
        decoder->hardware_input = NULL;
        decoder->hardware_input_transferred = false;
        if (source == NULL || source->rgb565_data == NULL ||
            source->display_width < destination->width ||
            source->display_height < destination->height ||
            source->rgb565_size < destination->width * destination->height *
                                      2U) {
            uint64_t release_start_us = jpeg_codec_now_us();

            jpeg_hardware_release_video_decoder(decoder);
            if (timing != NULL) {
                timing->decoder_release_time_us +=
                    jpeg_codec_now_us() - release_start_us;
            }
            return false;
        }
        {
            uint64_t convert_start_us = jpeg_codec_now_us();

            for (row = 0U; row < destination->height; ++row) {
                const uint8_t *input = source->rgb565_data +
                                       (size_t)row * source->width * 2U;
                uint8_t *output = destination->pixels +
                                  (size_t)row * destination->stride;
                uint32_t column;

                for (column = 0U; column < destination->width; ++column) {
                    uint16_t pixel = (uint16_t)input[column * 2U] |
                                     (uint16_t)input[column * 2U + 1U] << 8U;
                    uint8_t *target = output + (size_t)column * 4U;

                    target[0] = (uint8_t)(((pixel >> 11U) & 0x1FU) << 3U);
                    target[1] = (uint8_t)(((pixel >> 5U) & 0x3FU) << 2U);
                    target[2] = (uint8_t)((pixel & 0x1FU) << 3U);
                    target[3] = 255U;
                }
            }
            if (timing != NULL) {
                timing->format_convert_time_us +=
                    jpeg_codec_now_us() - convert_start_us;
            }
        }
        /* JpegDecoderGetFrame creates a new VideoDecoder internally.  Release
         * that child here so the long-lived JpegDecoder context does not
         * accumulate one Cedar decoder per camera frame. */
        {
            uint64_t release_start_us = jpeg_codec_now_us();

            jpeg_hardware_release_video_decoder(decoder);
            if (timing != NULL) {
                timing->decoder_release_time_us +=
                    jpeg_codec_now_us() - release_start_us;
            }
        }
        return true;
    }
    if (setjmp(decoder->error.jump) != 0) {
        decoder->api.abort_decompress(&decoder->cinfo);
        LOG_DEBUG("JPEG decode failed: %s", decoder->error.message);
        return false;
    }
    decoder->api.mem_src(&decoder->cinfo, jpeg_data, (unsigned long)jpeg_size);
    if (decoder->api.read_header(&decoder->cinfo, TRUE) != JPEG_HEADER_OK) {
        decoder->api.abort_decompress(&decoder->cinfo);
        return false;
    }
    decoder->cinfo.out_color_space = JCS_RGB;
    if (!decoder->api.start_decompress(&decoder->cinfo) ||
        decoder->cinfo.output_width != destination->width ||
        decoder->cinfo.output_height != destination->height ||
        decoder->cinfo.output_components != 3 ||
        (size_t)decoder->cinfo.output_width * 3U > decoder->row_capacity) {
        decoder->api.abort_decompress(&decoder->cinfo);
        return false;
    }
    for (row = 0U; row < destination->height; ++row) {
        JSAMPROW scanline = decoder->row_rgb;
        uint32_t column;

        if (decoder->api.read_scanlines(&decoder->cinfo, &scanline, 1U) !=
            1U) {
            decoder->api.abort_decompress(&decoder->cinfo);
            return false;
        }
        for (column = 0U; column < destination->width; ++column) {
            const uint8_t *source = decoder->row_rgb + (size_t)column * 3U;
            uint8_t *target = destination->pixels +
                              (size_t)row * destination->stride +
                              (size_t)column * VIDEO_FRAME_CHANNELS;

            target[0] = source[0];
            target[1] = source[1];
            target[2] = source[2];
            target[3] = 255U;
        }
    }
    if (!decoder->api.finish_decompress(&decoder->cinfo)) {
        decoder->api.abort_decompress(&decoder->cinfo);
        return false;
    }
    return true;
}

bool jpeg_decoder_decode(JpegDecoder *decoder, const uint8_t *jpeg_data,
                         size_t jpeg_size, VideoFrame *destination)
{
    return jpeg_decoder_decode_timed(decoder, jpeg_data, jpeg_size,
                                     destination, NULL);
}

static void jpeg_encoder_init_destination(j_compress_ptr cinfo)
{
    JpegEncoder *encoder = (JpegEncoder *)
        ((char *)cinfo->dest - offsetof(JpegEncoder, destination));

    encoder->destination.public_destination.next_output_byte = encoder->output;
    encoder->destination.public_destination.free_in_buffer =
        encoder->output_capacity;
}

static boolean jpeg_encoder_empty_output_buffer(j_compress_ptr cinfo)
{
    (void)cinfo;
    return FALSE;
}

static void jpeg_encoder_term_destination(j_compress_ptr cinfo)
{
    JpegEncoder *encoder = (JpegEncoder *)
        ((char *)cinfo->dest - offsetof(JpegEncoder, destination));

    encoder->output_size = encoder->output_capacity -
                           encoder->destination.public_destination.free_in_buffer;
}

int jpeg_encoder_init(JpegEncoder *encoder, uint32_t width,
                      size_t output_capacity)
{
    if (encoder == NULL || width == 0U || output_capacity == 0U ||
        (uint64_t)width * 3U > SIZE_MAX) {
        errno = EINVAL;
        return -1;
    }
    memset(encoder, 0, sizeof(*encoder));
    encoder->output = malloc(output_capacity);
    if (encoder->output == NULL) {
        return -1;
    }
    encoder->output_capacity = output_capacity;
    if (jpeg_codec_load(&encoder->api) < 0) {
        free(encoder->output);
        memset(encoder, 0, sizeof(*encoder));
        LOG_WARN("JPEG encoder unavailable; libjpeg runtime was not found");
        return -1;
    }
    encoder->row_rgb = malloc((size_t)width * 3U);
    if (encoder->row_rgb == NULL) {
        jpeg_codec_unload(&encoder->api);
        free(encoder->output);
        memset(encoder, 0, sizeof(*encoder));
        return -1;
    }
    encoder->row_capacity = (size_t)width * 3U;
    if (!jpeg_codec_prepare_error(&encoder->api, &encoder->error,
                                  (j_common_ptr)&encoder->cinfo)) {
        jpeg_codec_unload(&encoder->api);
        free(encoder->row_rgb);
        free(encoder->output);
        memset(encoder, 0, sizeof(*encoder));
        errno = EINVAL;
        return -1;
    }
    encoder->api.create_compress(&encoder->cinfo, encoder->api.library_version,
                                 sizeof(encoder->cinfo));
    encoder->destination.owner = encoder;
    encoder->destination.public_destination.init_destination =
        jpeg_encoder_init_destination;
    encoder->destination.public_destination.empty_output_buffer =
        jpeg_encoder_empty_output_buffer;
    encoder->destination.public_destination.term_destination =
        jpeg_encoder_term_destination;
    encoder->cinfo.dest = &encoder->destination.public_destination;
    encoder->initialized = true;
    return 0;
}

void jpeg_encoder_close(JpegEncoder *encoder)
{
    if (encoder == NULL) {
        return;
    }
    if (encoder->initialized) {
        encoder->api.destroy_compress(&encoder->cinfo);
    }
    jpeg_codec_unload(&encoder->api);
    free(encoder->row_rgb);
    free(encoder->output);
    memset(encoder, 0, sizeof(*encoder));
}

bool jpeg_encoder_encode_rgba(JpegEncoder *encoder, const VideoFrame *frame,
                              int quality, const uint8_t **jpeg_data,
                              size_t *jpeg_size)
{
    uint32_t row;

    if (encoder == NULL || !encoder->initialized || frame == NULL ||
        frame->pixels == NULL || jpeg_data == NULL || jpeg_size == NULL ||
        quality < 1 || quality > 100) {
        return false;
    }
    if (setjmp(encoder->error.jump) != 0) {
        encoder->api.abort_compress(&encoder->cinfo);
        LOG_DEBUG("JPEG encode failed: %s", encoder->error.message);
        return false;
    }
    encoder->output_size = 0U;
    encoder->cinfo.dest = &encoder->destination.public_destination;
    encoder->cinfo.client_data = &encoder->error;
    encoder->cinfo.image_width = frame->width;
    encoder->cinfo.image_height = frame->height;
    encoder->cinfo.input_components = 3;
    encoder->cinfo.in_color_space = JCS_RGB;
    encoder->api.set_defaults(&encoder->cinfo);
    encoder->api.set_quality(&encoder->cinfo, quality, TRUE);
    encoder->api.start_compress(&encoder->cinfo, TRUE);
    for (row = 0U; row < frame->height; ++row) {
        uint8_t *rgb = encoder->row_rgb;
        const uint8_t *source = frame->pixels + (size_t)row * frame->stride;
        uint32_t column;
        JSAMPROW scanline;

        if ((size_t)frame->width * 3U > encoder->row_capacity) {
            encoder->api.abort_compress(&encoder->cinfo);
            return false;
        }
        for (column = 0U; column < frame->width; ++column) {
            rgb[(size_t)column * 3U] = source[(size_t)column * 4U];
            rgb[(size_t)column * 3U + 1U] = source[(size_t)column * 4U + 1U];
            rgb[(size_t)column * 3U + 2U] = source[(size_t)column * 4U + 2U];
        }
        scanline = rgb;
        if (encoder->api.write_scanlines(&encoder->cinfo, &scanline, 1U) !=
            1U) {
            encoder->api.abort_compress(&encoder->cinfo);
            return false;
        }
    }
    encoder->api.finish_compress(&encoder->cinfo);
    *jpeg_data = encoder->output;
    *jpeg_size = encoder->output_size;
    return *jpeg_size > 0U;
}
