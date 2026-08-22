#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "g2d_driver.h"

#define G2D_DEVICE "/dev/g2d"
#define AWION_LIBRARY "libawion.so"
#define DEFAULT_WIDTH 1280U
#define DEFAULT_HEIGHT 720U
#define DEFAULT_ITERATIONS 4096U
#define VERIFY_INTERVAL 512U

typedef enum {
    SMOKE_OPERATION_COPY = 0,
    SMOKE_OPERATION_YUV_TO_ARGB,
    SMOKE_OPERATION_ARGB_TO_YUV,
    SMOKE_OPERATION_ARGB_TO_NV12,
    SMOKE_OPERATION_ARGB_TO_NV21,
    SMOKE_OPERATION_BLEND,
    SMOKE_OPERATION_YUV_BLEND
} SmokeOperation;

typedef int (*IonOpenFn)(void);
typedef void (*IonCloseFn)(void);
typedef void *(*IonPallocFn)(int size);
typedef void (*IonPfreeFn)(void *buffer);
typedef int (*IonGetBufferFdFn)(void *buffer);
typedef void (*IonFlushCacheFn)(void *buffer, int size);

typedef struct {
    void *library;
    IonOpenFn open;
    IonCloseFn close;
    IonPallocFn palloc;
    IonPfreeFn pfree;
    IonGetBufferFdFn get_buffer_fd;
    IonFlushCacheFn flush_cache;
} IonApi;

typedef struct {
    unsigned int iterations;
    const char *output_path;
    SmokeOperation operation;
} SmokeOptions;

typedef struct {
    void *memory;
    int fd;
    size_t size;
} IonBuffer;

static bool resolve_symbol(void *library, const char *name,
                           void *destination, size_t destination_size)
{
    void *symbol;

    symbol = dlsym(library, name);
    if (symbol == NULL) {
        fprintf(stderr, "missing %s: %s\n", name, dlerror());
        return false;
    }

    if (destination_size != sizeof(symbol)) {
        fprintf(stderr, "invalid function pointer slot for %s\n", name);
        return false;
    }
    memcpy(destination, &symbol, sizeof(symbol));
    return true;
}

static bool ion_load(IonApi *api)
{
    memset(api, 0, sizeof(*api));
    api->library = dlopen(AWION_LIBRARY, RTLD_NOW | RTLD_LOCAL);
    if (api->library == NULL) {
        fprintf(stderr, "cannot load %s: %s\n", AWION_LIBRARY, dlerror());
        return false;
    }

    if (!resolve_symbol(api->library, "sunxi_ion_alloc_open",
                        &api->open, sizeof(api->open)) ||
        !resolve_symbol(api->library, "sunxi_ion_alloc_close",
                        &api->close, sizeof(api->close)) ||
        !resolve_symbol(api->library, "sunxi_ion_alloc_palloc",
                        &api->palloc, sizeof(api->palloc)) ||
        !resolve_symbol(api->library, "sunxi_ion_alloc_pfree",
                        &api->pfree, sizeof(api->pfree)) ||
        !resolve_symbol(api->library, "sunxi_ion_alloc_get_bufferFd",
                        &api->get_buffer_fd, sizeof(api->get_buffer_fd)) ||
        !resolve_symbol(api->library, "sunxi_ion_alloc_flush_cache",
                        &api->flush_cache, sizeof(api->flush_cache))) {
        dlclose(api->library);
        memset(api, 0, sizeof(*api));
        return false;
    }
    return true;
}

static void ion_unload(IonApi *api)
{
    if (api->library != NULL) {
        dlclose(api->library);
    }
    memset(api, 0, sizeof(*api));
}

static void print_usage(const char *program)
{
    printf("Usage: %s [--operation OP] [--iterations N] [--output FILE]\n",
           program);
    printf("  OP: copy, yuv-to-argb, argb-to-yuv, argb-to-nv12, "
           "argb-to-nv21, blend, yuv-blend.\n");
    printf("  copy runs ARGB8888 dma-buf -> dma-buf G2D BITBLT_H.\n");
    printf("  Defaults: 1280x720, %u iterations.\n", DEFAULT_ITERATIONS);
}

static const char *operation_name(SmokeOperation operation)
{
    switch (operation) {
    case SMOKE_OPERATION_COPY:
        return "copy";
    case SMOKE_OPERATION_YUV_TO_ARGB:
        return "yuv-to-argb";
    case SMOKE_OPERATION_ARGB_TO_YUV:
        return "argb-to-yuv";
    case SMOKE_OPERATION_ARGB_TO_NV12:
        return "argb-to-nv12";
    case SMOKE_OPERATION_ARGB_TO_NV21:
        return "argb-to-nv21";
    case SMOKE_OPERATION_BLEND:
        return "blend";
    case SMOKE_OPERATION_YUV_BLEND:
        return "yuv-blend";
    default:
        return "unknown";
    }
}

static bool parse_operation(const char *value, SmokeOperation *operation)
{
    if (strcmp(value, "copy") == 0) {
        *operation = SMOKE_OPERATION_COPY;
    } else if (strcmp(value, "yuv-to-argb") == 0) {
        *operation = SMOKE_OPERATION_YUV_TO_ARGB;
    } else if (strcmp(value, "argb-to-yuv") == 0) {
        *operation = SMOKE_OPERATION_ARGB_TO_YUV;
    } else if (strcmp(value, "argb-to-nv12") == 0) {
        *operation = SMOKE_OPERATION_ARGB_TO_NV12;
    } else if (strcmp(value, "argb-to-nv21") == 0) {
        *operation = SMOKE_OPERATION_ARGB_TO_NV21;
    } else if (strcmp(value, "blend") == 0) {
        *operation = SMOKE_OPERATION_BLEND;
    } else if (strcmp(value, "yuv-blend") == 0) {
        *operation = SMOKE_OPERATION_YUV_BLEND;
    } else {
        return false;
    }
    return true;
}

static bool parse_options(int argc, char **argv, SmokeOptions *options)
{
    int index;

    options->iterations = DEFAULT_ITERATIONS;
    options->output_path = NULL;
    options->operation = SMOKE_OPERATION_COPY;
    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--help") == 0 ||
            strcmp(argv[index], "-h") == 0) {
            print_usage(argv[0]);
            exit(EXIT_SUCCESS);
        }
        if (strcmp(argv[index], "--iterations") == 0 && index + 1 < argc) {
            char *end = NULL;
            unsigned long value;

            errno = 0;
            value = strtoul(argv[++index], &end, 10);
            if (errno != 0 || end == argv[index] || *end != '\0' ||
                value == 0UL || value > UINT32_MAX) {
                fprintf(stderr, "invalid --iterations value\n");
                return false;
            }
            options->iterations = (unsigned int)value;
            continue;
        }
        if (strcmp(argv[index], "--operation") == 0 && index + 1 < argc) {
            if (!parse_operation(argv[++index], &options->operation)) {
                fprintf(stderr, "invalid --operation value: %s\n",
                        argv[index]);
                return false;
            }
            continue;
        }
        if (strcmp(argv[index], "--output") == 0 && index + 1 < argc) {
            options->output_path = argv[++index];
            continue;
        }
        fprintf(stderr, "unknown or incomplete option: %s\n", argv[index]);
        return false;
    }
    return true;
}

static void fill_pattern(uint32_t *pixels, unsigned int width,
                         unsigned int height)
{
    unsigned int y;

    for (y = 0U; y < height; ++y) {
        unsigned int x;

        for (x = 0U; x < width; ++x) {
            uint32_t red = (uint32_t)(x & 0xffU);
            uint32_t green = (uint32_t)(y & 0xffU);
            uint32_t blue = (uint32_t)((x + y) & 0xffU);

            pixels[(size_t)y * width + x] = 0xff000000U |
                                             (red << 16U) |
                                             (green << 8U) |
                                             blue;
        }
    }
}

static void fill_yuv420_pattern(uint8_t *pixels, unsigned int width,
                                unsigned int height)
{
    size_t luma_size = (size_t)width * height;
    size_t chroma_size = luma_size / 4U;
    size_t index;

    for (index = 0U; index < luma_size; ++index) {
        unsigned int x = (unsigned int)(index % width);
        unsigned int y = (unsigned int)(index / width);

        pixels[index] = (uint8_t)(32U + ((x + y) & 0x7fU));
    }
    /* g2d_driver.h's YUV420_PLANAR path maps laddr[1] to V and
     * laddr[2] to U.  Keep that order explicit in this test pattern. */
    for (index = 0U; index < chroma_size; ++index) {
        pixels[luma_size + index] = (uint8_t)(192U + (index & 0x1fU));
        pixels[luma_size + chroma_size + index] =
            (uint8_t)(64U + (index & 0x1fU));
    }
}

static void fill_blend_foreground(uint32_t *pixels, unsigned int width,
                                  unsigned int height)
{
    unsigned int y;

    for (y = 0U; y < height; ++y) {
        unsigned int x;

        for (x = 0U; x < width; ++x) {
            uint32_t alpha = (x < width / 2U) ? 0x80U : 0xffU;
            pixels[(size_t)y * width + x] = (alpha << 24U) |
                                             0x00e04020U;
        }
    }
}

static void fill_blend_background(uint32_t *pixels, unsigned int width,
                                  unsigned int height)
{
    unsigned int y;

    for (y = 0U; y < height; ++y) {
        unsigned int x;

        for (x = 0U; x < width; ++x) {
            pixels[(size_t)y * width + x] = 0xff204080U;
        }
    }
}

static uint32_t checksum32(const uint8_t *data, size_t size)
{
    size_t index;
    uint32_t checksum = 2166136261U;

    for (index = 0U; index < size; ++index) {
        checksum ^= data[index];
        checksum *= 16777619U;
    }
    return checksum;
}

static bool verify_copy(const uint8_t *source, const uint8_t *destination,
                        size_t size)
{
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (source[index] != destination[index]) {
            fprintf(stderr,
                    "copy mismatch at byte %zu: source=0x%02x "
                    "destination=0x%02x\n",
                    index, source[index], destination[index]);
            return false;
        }
    }
    return true;
}

static void set_image_defaults(g2d_image_enh *image, int buffer_fd,
                               g2d_fmt_enh format, unsigned int width,
                               unsigned int height)
{
    memset(image, 0, sizeof(*image));
    image->bbuff = 1;
    image->format = format;
    image->width = width;
    image->height = height;
    image->clip_rect.x = 0;
    image->clip_rect.y = 0;
    image->clip_rect.w = width;
    image->clip_rect.h = height;
    image->gamut = G2D_BT601;
    image->bpremul = 0;
    image->alpha = 0xffU;
    image->mode = G2D_PIXEL_ALPHA;
    image->fd = buffer_fd;
    image->use_phy_addr = 0U;
    image->color_range = COLOR_RANGE_0_255;
}

static size_t image_size_for_format(g2d_fmt_enh format, unsigned int width,
                                    unsigned int height)
{
    if (format == G2D_FORMAT_ARGB8888) {
        return (size_t)width * height * 4U;
    }
    if (format == G2D_FORMAT_YUV420_PLANAR ||
        format == G2D_FORMAT_YUV420UVC_V1U1V0U0 ||
        format == G2D_FORMAT_YUV420UVC_U1V1U0V0) {
        return (size_t)width * height * 3U / 2U;
    }
    return 0U;
}

static void ion_buffer_reset(IonBuffer *buffer)
{
    memset(buffer, 0, sizeof(*buffer));
    buffer->fd = -1;
}

static bool ion_buffer_alloc(const IonApi *ion, IonBuffer *buffer,
                             size_t size)
{
    ion_buffer_reset(buffer);
    if (size == 0U || size > INT_MAX) {
        return false;
    }
    buffer->memory = ion->palloc((int)size);
    if (buffer->memory == NULL) {
        return false;
    }
    buffer->fd = ion->get_buffer_fd(buffer->memory);
    buffer->size = size;
    if (buffer->fd < 0) {
        ion->pfree(buffer->memory);
        ion_buffer_reset(buffer);
        return false;
    }
    return true;
}

static void ion_buffer_free(const IonApi *ion, IonBuffer *buffer)
{
    if (buffer->memory != NULL) {
        ion->pfree(buffer->memory);
    }
    ion_buffer_reset(buffer);
}

static void flush_buffer(const IonApi *ion, const IonBuffer *buffer)
{
    if (buffer->memory != NULL && buffer->size <= INT_MAX) {
        ion->flush_cache(buffer->memory, (int)buffer->size);
    }
}

static bool buffer_has_nonzero_data(const IonBuffer *buffer)
{
    size_t index;
    const uint8_t *data = (const uint8_t *)buffer->memory;

    for (index = 0U; index < buffer->size; ++index) {
        if (data[index] != 0U) {
            return true;
        }
    }
    return false;
}

static bool run_bitblt(int g2d_fd, g2d_fmt_enh source_format,
                       int source_fd, g2d_fmt_enh destination_format,
                       int destination_fd, unsigned int width,
                       unsigned int height, unsigned int iteration)
{
    g2d_blt_h blt;
    int result;

    memset(&blt, 0, sizeof(blt));
    /* The RCQ driver uses the NONE_H path for format conversion. ROT_0 returns
     * success on this board but leaves YUV-to-ARGB output in a raw planar
     * layout instead of converting it. */
    blt.flag_h = source_format == destination_format ? G2D_BLT_COPYPEN :
                  G2D_BLT_NONE_H;
    set_image_defaults(&blt.src_image_h, source_fd, source_format, width,
                       height);
    set_image_defaults(&blt.dst_image_h, destination_fd, destination_format,
                       width, height);
    errno = 0;
    result = ioctl(g2d_fd, G2D_CMD_BITBLT_H, &blt);
    if (result < 0) {
        fprintf(stderr,
                "BITBLT_H failed at iteration %u: cmd=0x%x src_fd=%d "
                "dst_fd=%d src_format=0x%x dst_format=0x%x errno=%d (%s)\n",
                iteration, G2D_CMD_BITBLT_H, source_fd, destination_fd,
                source_format, destination_format, errno, strerror(errno));
        return false;
    }
    return true;
}

static void print_image_summary(const char *name, const g2d_image_enh *image)
{
    printf("  %s: fd=%d phy=%u format=0x%x size=%ux%u "
           "align=[%u,%u,%u] clip=[%d,%d,%d,%d] alpha=%u mode=%d "
           "bbuff=%d gamut=%d range=%d\n",
           name, image->fd, image->use_phy_addr, image->format,
           image->width, image->height, image->align[0], image->align[1],
           image->align[2], image->clip_rect.x, image->clip_rect.y,
           image->clip_rect.w, image->clip_rect.h, image->alpha, image->mode,
           image->bbuff, image->gamut, image->color_range);
}

static bool run_blend(int g2d_fd, g2d_fmt_enh background_format,
                      int background_fd, int overlay_fd,
                      g2d_fmt_enh destination_format, int destination_fd,
                      unsigned int width, unsigned int height,
                      unsigned int iteration)
{
    g2d_bld blend;
    int result;

    memset(&blend, 0, sizeof(blend));
    blend.bld_cmd = G2D_BLD_SRCOVER;
    set_image_defaults(&blend.src_image[0], background_fd, background_format,
                       width, height);
    set_image_defaults(&blend.src_image[1], overlay_fd, G2D_FORMAT_ARGB8888,
                       width, height);
    set_image_defaults(&blend.dst_image, destination_fd, destination_format,
                       width, height);
    if (iteration == 1U) {
        printf("BLD_H parameters: cmd=0x%x sizeof(g2d_bld)=%zu "
               "sizeof(g2d_image_enh)=%zu\n",
               G2D_CMD_BLD_H, sizeof(g2d_bld), sizeof(g2d_image_enh));
        print_image_summary("src_image[0] background", &blend.src_image[0]);
        print_image_summary("src_image[1] overlay", &blend.src_image[1]);
        print_image_summary("src_image[2] unused", &blend.src_image[2]);
        print_image_summary("src_image[3] unused", &blend.src_image[3]);
        print_image_summary("dst_image", &blend.dst_image);
    }
    errno = 0;
    result = ioctl(g2d_fd, G2D_CMD_BLD_H, &blend);
    if (result < 0) {
        fprintf(stderr,
                "BLD_H failed at iteration %u: cmd=0x%x src0_fd=%d "
                "src1_fd=%d src2_fd=%d src3_fd=%d dst_fd=%d "
                "dst_format=0x%x errno=%d (%s)\n",
                iteration, G2D_CMD_BLD_H, blend.src_image[0].fd,
                blend.src_image[1].fd, blend.src_image[2].fd,
                blend.src_image[3].fd, blend.dst_image.fd, destination_format,
                errno, strerror(errno));
        return false;
    }
    return true;
}

static bool write_output(const char *path, const uint8_t *data, size_t size)
{
    FILE *file;

    if (path == NULL) {
        return true;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "cannot open output %s: %s\n", path,
                strerror(errno));
        return false;
    }
    if (fwrite(data, 1U, size, file) != size || fclose(file) != 0) {
        fprintf(stderr, "cannot write output %s\n", path);
        return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    const unsigned int width = DEFAULT_WIDTH;
    const unsigned int height = DEFAULT_HEIGHT;
    SmokeOptions options;
    IonApi ion;
    IonBuffer source;
    IonBuffer overlay;
    IonBuffer destination;
    g2d_fmt_enh source_format = G2D_FORMAT_ARGB8888;
    g2d_fmt_enh overlay_format = G2D_FORMAT_ARGB8888;
    g2d_fmt_enh destination_format = G2D_FORMAT_ARGB8888;
    int g2d_fd = -1;
    bool ion_opened = false;
    bool success = false;
    unsigned int iteration;
    size_t source_size;
    size_t overlay_size;
    size_t destination_size;

    ion_buffer_reset(&source);
    ion_buffer_reset(&overlay);
    ion_buffer_reset(&destination);

    if (!parse_options(argc, argv, &options)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    switch (options.operation) {
    case SMOKE_OPERATION_YUV_TO_ARGB:
        source_format = G2D_FORMAT_YUV420_PLANAR;
        break;
    case SMOKE_OPERATION_ARGB_TO_YUV:
        destination_format = G2D_FORMAT_YUV420_PLANAR;
        break;
    case SMOKE_OPERATION_ARGB_TO_NV12:
        destination_format = G2D_FORMAT_YUV420UVC_U1V1U0V0;
        break;
    case SMOKE_OPERATION_ARGB_TO_NV21:
        destination_format = G2D_FORMAT_YUV420UVC_V1U1V0U0;
        break;
    case SMOKE_OPERATION_YUV_BLEND:
        source_format = G2D_FORMAT_YUV420_PLANAR;
        destination_format = G2D_FORMAT_YUV420_PLANAR;
        break;
    default:
        break;
    }
    source_size = image_size_for_format(source_format, width, height);
    overlay_size = image_size_for_format(overlay_format, width, height);
    destination_size = image_size_for_format(destination_format, width, height);
    printf("g2d smoke: operation=%s %ux%u, iterations=%u\n",
           operation_name(options.operation), width, height,
           options.iterations);
    printf("UAPI: image=%zu, blt_h=%zu, bld=%zu, mixer=%zu, "
           "bitblt_cmd=0x%x bld_cmd=0x%x\n",
           sizeof(g2d_image_enh), sizeof(g2d_blt_h), sizeof(g2d_bld),
           sizeof(struct mixer_para), G2D_CMD_BITBLT_H, G2D_CMD_BLD_H);
    if (source_size == 0U || destination_size == 0U) {
        fprintf(stderr, "unsupported smoke format\n");
        return EXIT_FAILURE;
    }

    if (!ion_load(&ion)) {
        return EXIT_FAILURE;
    }
    if (ion.open() != 0) {
        fprintf(stderr, "sunxi_ion_alloc_open failed\n");
        goto cleanup;
    }
    ion_opened = true;

    if (!ion_buffer_alloc(&ion, &source, source_size) ||
        ((options.operation == SMOKE_OPERATION_BLEND ||
          options.operation == SMOKE_OPERATION_YUV_BLEND) &&
         !ion_buffer_alloc(&ion, &overlay, overlay_size)) ||
        !ion_buffer_alloc(&ion, &destination, destination_size)) {
        fprintf(stderr, "libawion allocation failed\n");
        goto cleanup;
    }

    if (options.operation == SMOKE_OPERATION_YUV_BLEND) {
        fill_yuv420_pattern((uint8_t *)source.memory, width, height);
        fill_blend_foreground((uint32_t *)overlay.memory, width, height);
    } else if (options.operation == SMOKE_OPERATION_BLEND) {
        fill_blend_background((uint32_t *)source.memory, width, height);
        fill_blend_foreground((uint32_t *)overlay.memory, width, height);
    } else if (source_format == G2D_FORMAT_YUV420_PLANAR) {
        fill_yuv420_pattern((uint8_t *)source.memory, width, height);
    } else {
        fill_pattern((uint32_t *)source.memory, width, height);
    }
    memset(destination.memory, 0, destination_size);
    flush_buffer(&ion, &source);
    flush_buffer(&ion, &overlay);
    flush_buffer(&ion, &destination);

    g2d_fd = open(G2D_DEVICE, O_RDWR | O_CLOEXEC);
    if (g2d_fd < 0) {
        fprintf(stderr, "cannot open %s: %s\n", G2D_DEVICE, strerror(errno));
        goto cleanup;
    }
    printf("buffers: src_fd=%d dst_fd=%d g2d_fd=%d\n",
           source.fd, destination.fd, g2d_fd);

    for (iteration = 0U; iteration < options.iterations; ++iteration) {
        bool operation_ok;

        if (options.operation == SMOKE_OPERATION_BLEND ||
            options.operation == SMOKE_OPERATION_YUV_BLEND) {
            operation_ok = run_blend(g2d_fd, source_format, source.fd,
                                     overlay.fd, destination_format,
                                     destination.fd, width, height,
                                     iteration + 1U);
        } else {
            operation_ok = run_bitblt(g2d_fd, source_format, source.fd,
                                      destination_format, destination.fd,
                                      width, height, iteration + 1U);
        }
        if (!operation_ok) {
            goto cleanup;
        }
        flush_buffer(&ion, &destination);

        if (iteration == 0U ||
            iteration + 1U == options.iterations ||
            (iteration + 1U) % VERIFY_INTERVAL == 0U) {
            if (options.operation == SMOKE_OPERATION_COPY &&
                !verify_copy((const uint8_t *)source.memory,
                             (const uint8_t *)destination.memory,
                             source.size)) {
                goto cleanup;
            }
            if (options.operation != SMOKE_OPERATION_COPY &&
                !buffer_has_nonzero_data(&destination)) {
                fprintf(stderr,
                        "destination remained all zero at iteration %u\n",
                        iteration + 1U);
                goto cleanup;
            }
            printf("verified iteration %u, checksum=0x%08" PRIx32 "\n",
                   iteration + 1U,
                   checksum32((const uint8_t *)destination.memory,
                              destination.size));
        }
    }

    if (!write_output(options.output_path, (const uint8_t *)destination.memory,
                      destination.size)) {
        goto cleanup;
    }
    printf("PASS: %u %s operations\n", options.iterations,
           operation_name(options.operation));
    success = true;

cleanup:
    if (g2d_fd >= 0) {
        close(g2d_fd);
    }
    ion_buffer_free(&ion, &destination);
    ion_buffer_free(&ion, &overlay);
    ion_buffer_free(&ion, &source);
    if (ion_opened) {
        ion.close();
    }
    ion_unload(&ion);
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
