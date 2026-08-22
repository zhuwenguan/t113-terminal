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
#include <sys/ioctl.h>
#include <unistd.h>

#include "g2d_driver.h"

#define G2D_DEVICE "/dev/g2d"
#define AWION_LIBRARY "libawion.so"
#define DEFAULT_WIDTH 1280U
#define DEFAULT_HEIGHT 720U
#define DEFAULT_ITERATIONS 4096U
#define VERIFY_INTERVAL 512U

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
} SmokeOptions;

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
    printf("Usage: %s [--iterations N] [--output FILE]\n", program);
    printf("  Runs ARGB8888 dma-buf -> dma-buf G2D BITBLT_H.\n");
    printf("  Defaults: 1280x720, %u iterations.\n", DEFAULT_ITERATIONS);
}

static bool parse_options(int argc, char **argv, SmokeOptions *options)
{
    int index;

    options->iterations = DEFAULT_ITERATIONS;
    options->output_path = NULL;
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
                               unsigned int width, unsigned int height)
{
    memset(image, 0, sizeof(*image));
    image->bbuff = 1;
    image->format = G2D_FORMAT_ARGB8888;
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
    const size_t image_size = (size_t)width * height * 4U;
    SmokeOptions options;
    IonApi ion;
    void *source_buffer = NULL;
    void *destination_buffer = NULL;
    int source_fd = -1;
    int destination_fd = -1;
    int g2d_fd = -1;
    bool ion_opened = false;
    bool success = false;
    unsigned int iteration;

    if (!parse_options(argc, argv, &options)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    printf("g2d smoke: %ux%u, iterations=%u\n",
           width, height, options.iterations);
    printf("UAPI: image=%zu, blt_h=%zu, mixer=%zu, cmd=0x%x\n",
           sizeof(g2d_image_enh), sizeof(g2d_blt_h), sizeof(struct mixer_para),
           G2D_CMD_BITBLT_H);

    if (!ion_load(&ion)) {
        return EXIT_FAILURE;
    }
    if (ion.open() != 0) {
        fprintf(stderr, "sunxi_ion_alloc_open failed\n");
        goto cleanup;
    }
    ion_opened = true;

    source_buffer = ion.palloc((int)image_size);
    destination_buffer = ion.palloc((int)image_size);
    if (source_buffer == NULL || destination_buffer == NULL) {
        fprintf(stderr, "libawion allocation failed\n");
        goto cleanup;
    }
    source_fd = ion.get_buffer_fd(source_buffer);
    destination_fd = ion.get_buffer_fd(destination_buffer);
    if (source_fd < 0 || destination_fd < 0) {
        fprintf(stderr, "invalid dma-buf fd: src=%d dst=%d\n",
                source_fd, destination_fd);
        goto cleanup;
    }

    fill_pattern((uint32_t *)source_buffer, width, height);
    memset(destination_buffer, 0, image_size);
    ion.flush_cache(source_buffer, (int)image_size);
    ion.flush_cache(destination_buffer, (int)image_size);

    g2d_fd = open(G2D_DEVICE, O_RDWR | O_CLOEXEC);
    if (g2d_fd < 0) {
        fprintf(stderr, "cannot open %s: %s\n", G2D_DEVICE, strerror(errno));
        goto cleanup;
    }
    printf("buffers: src_fd=%d dst_fd=%d g2d_fd=%d\n",
           source_fd, destination_fd, g2d_fd);

    for (iteration = 0U; iteration < options.iterations; ++iteration) {
        g2d_blt_h blt;
        int result;

        memset(&blt, 0, sizeof(blt));
        blt.flag_h = G2D_BLT_COPYPEN;
        set_image_defaults(&blt.src_image_h, source_fd, width, height);
        set_image_defaults(&blt.dst_image_h, destination_fd, width, height);

        errno = 0;
        result = ioctl(g2d_fd, G2D_CMD_BITBLT_H, &blt);
        if (result < 0) {
            fprintf(stderr,
                    "BITBLT_H failed at iteration %u: cmd=0x%x "
                    "src_fd=%d dst_fd=%d errno=%d (%s)\n",
                    iteration, G2D_CMD_BITBLT_H, source_fd, destination_fd,
                    errno, strerror(errno));
            goto cleanup;
        }
        ion.flush_cache(destination_buffer, (int)image_size);

        if (iteration == 0U ||
            iteration + 1U == options.iterations ||
            (iteration + 1U) % VERIFY_INTERVAL == 0U) {
            if (!verify_copy((const uint8_t *)source_buffer,
                             (const uint8_t *)destination_buffer,
                             image_size)) {
                goto cleanup;
            }
            printf("verified iteration %u, checksum=0x%08" PRIx32 "\n",
                   iteration + 1U,
                   checksum32((const uint8_t *)destination_buffer,
                              image_size));
        }
    }

    if (!write_output(options.output_path, (const uint8_t *)destination_buffer,
                      image_size)) {
        goto cleanup;
    }
    printf("PASS: %u BITBLT_H operations\n", options.iterations);
    success = true;

cleanup:
    if (g2d_fd >= 0) {
        close(g2d_fd);
    }
    if (destination_buffer != NULL) {
        ion.pfree(destination_buffer);
    }
    if (source_buffer != NULL) {
        ion.pfree(source_buffer);
    }
    if (ion_opened) {
        ion.close();
    }
    ion_unload(&ion);
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
