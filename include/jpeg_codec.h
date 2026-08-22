#ifndef T113_JPEG_CODEC_H
#define T113_JPEG_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>

#include "video_frame.h"

#if defined(__arm__) || defined(__aarch64__)
#include "jpeglib.h"
#else
#include "host/jpeglib.h"
#endif

typedef struct {
    struct jpeg_error_mgr public_error;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
} JpegCodecError;

typedef struct {
    void *library_handle;
    int library_version;
    struct jpeg_error_mgr *(*std_error)(struct jpeg_error_mgr *error);
    int (*version)(void);
    void (*create_decompress)(j_decompress_ptr cinfo, int version,
                              size_t structure_size);
    void (*destroy_decompress)(j_decompress_ptr cinfo);
    void (*mem_src)(j_decompress_ptr cinfo, const unsigned char *buffer,
                    unsigned long size);
    int (*read_header)(j_decompress_ptr cinfo, boolean require_image);
    boolean (*start_decompress)(j_decompress_ptr cinfo);
    JDIMENSION (*read_scanlines)(j_decompress_ptr cinfo, JSAMPARRAY scanlines,
                                 JDIMENSION max_lines);
    boolean (*finish_decompress)(j_decompress_ptr cinfo);
    void (*abort_decompress)(j_decompress_ptr cinfo);
    void (*create_compress)(j_compress_ptr cinfo, int version,
                            size_t structure_size);
    void (*destroy_compress)(j_compress_ptr cinfo);
    void (*set_defaults)(j_compress_ptr cinfo);
    void (*set_quality)(j_compress_ptr cinfo, int quality,
                        boolean force_baseline);
    void (*start_compress)(j_compress_ptr cinfo, boolean write_all_tables);
    JDIMENSION (*write_scanlines)(j_compress_ptr cinfo, JSAMPARRAY scanlines,
                                  JDIMENSION num_lines);
    void (*finish_compress)(j_compress_ptr cinfo);
    void (*abort_compress)(j_compress_ptr cinfo);
} JpegCodecApi;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t display_width;
    uint32_t display_height;
    uint32_t yuv_size;
    uint8_t *yuv_data;
    uint32_t rgb565_size;
    uint8_t *rgb565_data;
    int32_t rotation;
} JpegHardwareFrame;

typedef struct {
    JpegCodecApi api;
    JpegCodecError error;
    struct jpeg_decompress_struct cinfo;
    uint8_t *row_rgb;
    size_t row_capacity;
    void *hardware_library;
    void *hardware_decoder;
    uint8_t *hardware_input;
    bool hardware_input_transferred;
    JpegHardwareFrame hardware_frame;
    bool initialized;
    bool hardware_initialized;
} JpegDecoder;

typedef struct {
    uint64_t input_copy_time_us;
    uint64_t decoder_submit_time_us;
    /* The current vendor GetFrame API combines decode and picture acquire. */
    uint64_t decoder_get_frame_time_us;
    uint64_t decoder_release_time_us;
    uint64_t format_convert_time_us;
} JpegDecoderTiming;

typedef struct {
    JpegCodecApi api;
    JpegCodecError error;
    struct jpeg_compress_struct cinfo;
    struct {
        struct jpeg_destination_mgr public_destination;
        void *owner;
    } destination;
    uint8_t *output;
    uint8_t *row_rgb;
    size_t row_capacity;
    size_t output_capacity;
    size_t output_size;
    bool initialized;
} JpegEncoder;

int jpeg_decoder_init(JpegDecoder *decoder, uint32_t width);
void jpeg_decoder_close(JpegDecoder *decoder);
bool jpeg_decoder_decode_timed(JpegDecoder *decoder, const uint8_t *jpeg_data,
                               size_t jpeg_size, VideoFrame *destination,
                               JpegDecoderTiming *timing);
bool jpeg_decoder_decode(JpegDecoder *decoder, const uint8_t *jpeg_data,
                         size_t jpeg_size, VideoFrame *destination);

int jpeg_encoder_init(JpegEncoder *encoder, uint32_t width,
                      size_t output_capacity);
void jpeg_encoder_close(JpegEncoder *encoder);
bool jpeg_encoder_encode_rgba(JpegEncoder *encoder, const VideoFrame *frame,
                              int quality, const uint8_t **jpeg_data,
                              size_t *jpeg_size);

#endif
