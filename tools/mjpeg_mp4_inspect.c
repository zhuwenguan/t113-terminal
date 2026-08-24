#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t offset;
    uint64_t size;
    uint64_t header_size;
    char type[5];
} Mp4TopLevelBox;

static uint32_t read_be32(const uint8_t *value)
{
    return ((uint32_t)value[0] << 24U) | ((uint32_t)value[1] << 16U) |
           ((uint32_t)value[2] << 8U) | value[3];
}

static uint64_t read_be64(const uint8_t *value)
{
    return ((uint64_t)read_be32(value) << 32U) | read_be32(value + 4U);
}

static bool read_at(FILE *file, uint64_t offset, void *data, size_t size)
{
    return offset <= (uint64_t)INT64_MAX &&
           fseeko(file, (off_t)offset, SEEK_SET) == 0 &&
           fread(data, 1U, size, file) == size;
}

static bool read_top_level_box(FILE *file, uint64_t file_size,
                               uint64_t offset, Mp4TopLevelBox *box)
{
    uint8_t header[16];
    uint32_t size32;

    if (box == NULL || offset > file_size || file_size - offset < 8U ||
        !read_at(file, offset, header, 8U)) {
        return false;
    }
    memset(box, 0, sizeof(*box));
    box->offset = offset;
    memcpy(box->type, header + 4U, 4U);
    size32 = read_be32(header);
    box->header_size = 8U;
    if (size32 == 1U) {
        if (file_size - offset < sizeof(header) ||
            !read_at(file, offset, header, sizeof(header))) {
            return false;
        }
        box->size = read_be64(header + 8U);
        box->header_size = 16U;
    } else if (size32 == 0U) {
        box->size = file_size - offset;
    } else {
        box->size = size32;
    }
    return box->size >= box->header_size && box->size <= file_size - offset;
}

static bool mp4_container_type(const uint8_t *type)
{
    static const char *const containers[] = {
        "moov", "trak", "mdia", "minf", "stbl", "dinf"
    };
    size_t index;

    for (index = 0U; index < sizeof(containers) / sizeof(containers[0]);
         ++index) {
        if (memcmp(type, containers[index], 4U) == 0) {
            return true;
        }
    }
    return false;
}

static const uint8_t *find_box(const uint8_t *data, size_t size,
                               const char target[4], size_t *box_size)
{
    size_t offset = 0U;

    while (size - offset >= 8U) {
        uint32_t child_size = read_be32(data + offset);
        const uint8_t *type = data + offset + 4U;
        const uint8_t *nested;
        size_t nested_size;

        if (child_size < 8U || child_size > size - offset) {
            return NULL;
        }
        if (memcmp(type, target, 4U) == 0) {
            *box_size = child_size;
            return data + offset;
        }
        if (mp4_container_type(type)) {
            nested = find_box(data + offset + 8U, child_size - 8U, target,
                              &nested_size);
            if (nested != NULL) {
                *box_size = nested_size;
                return nested;
            }
        }
        offset += child_size;
    }
    return NULL;
}

static bool sample_has_jpeg_markers(FILE *file, uint64_t offset,
                                    uint32_t size)
{
    uint8_t markers[4];

    if (size < 4U || !read_at(file, offset, markers, 2U) ||
        !read_at(file, offset + size - 2U, markers + 2U, 2U)) {
        return false;
    }
    return markers[0] == 0xffU && markers[1] == 0xd8U &&
           markers[2] == 0xffU && markers[3] == 0xd9U;
}

int main(int argc, char *argv[])
{
    Mp4TopLevelBox box;
    Mp4TopLevelBox mdat = {0};
    Mp4TopLevelBox moov = {0};
    FILE *file;
    uint8_t *movie = NULL;
    const uint8_t *mvhd;
    const uint8_t *stts;
    const uint8_t *stsz;
    const uint8_t *co64;
    size_t mvhd_size = 0U;
    size_t stts_size = 0U;
    size_t stsz_size = 0U;
    size_t co64_size = 0U;
    uint64_t file_size;
    uint64_t offset = 0U;
    uint64_t duration_sum = 0U;
    uint64_t timing_sample_count = 0U;
    uint32_t timescale;
    uint32_t movie_duration;
    uint32_t timing_entries;
    uint32_t sample_count;
    uint32_t chunk_count;
    uint32_t index;
    bool have_ftyp = false;
    int result = EXIT_FAILURE;

    if (argc != 2) {
        fprintf(stderr, "usage: %s FILE.mp4\n", argv[0]);
        return EXIT_FAILURE;
    }
    file = fopen(argv[1], "rb");
    if (file == NULL || fseeko(file, 0, SEEK_END) != 0 ||
        (offset = (uint64_t)ftello(file)) > (uint64_t)INT64_MAX) {
        fprintf(stderr, "cannot open %s: %s\n", argv[1], strerror(errno));
        if (file != NULL) {
            fclose(file);
        }
        return EXIT_FAILURE;
    }
    file_size = offset;
    offset = 0U;
    while (offset < file_size) {
        if (!read_top_level_box(file, file_size, offset, &box)) {
            fprintf(stderr, "invalid top-level box at offset=%" PRIu64 "\n",
                    offset);
            goto done;
        }
        if (memcmp(box.type, "ftyp", 4U) == 0) {
            have_ftyp = true;
        } else if (memcmp(box.type, "mdat", 4U) == 0) {
            mdat = box;
        } else if (memcmp(box.type, "moov", 4U) == 0) {
            moov = box;
        }
        offset += box.size;
    }
    if (!have_ftyp || mdat.size == 0U || moov.size < 8U ||
        moov.offset + moov.size != file_size || moov.size > SIZE_MAX) {
        fprintf(stderr, "missing ftyp/mdat/moov or moov is not file tail\n");
        goto done;
    }
    movie = malloc((size_t)moov.size);
    if (movie == NULL || !read_at(file, moov.offset, movie,
                                  (size_t)moov.size)) {
        fprintf(stderr, "cannot read moov box\n");
        goto done;
    }
    mvhd = find_box(movie + 8U, (size_t)moov.size - 8U, "mvhd",
                    &mvhd_size);
    stts = find_box(movie + 8U, (size_t)moov.size - 8U, "stts",
                    &stts_size);
    stsz = find_box(movie + 8U, (size_t)moov.size - 8U, "stsz",
                    &stsz_size);
    co64 = find_box(movie + 8U, (size_t)moov.size - 8U, "co64",
                    &co64_size);
    if (mvhd == NULL || mvhd_size < 28U || stts == NULL || stts_size < 16U ||
        stsz == NULL || stsz_size < 20U || co64 == NULL || co64_size < 16U ||
        read_be32(mvhd + 8U) != 0U) {
        fprintf(stderr, "missing or unsupported timing/sample tables\n");
        goto done;
    }
    timescale = read_be32(mvhd + 20U);
    movie_duration = read_be32(mvhd + 24U);
    timing_entries = read_be32(stts + 12U);
    sample_count = read_be32(stsz + 16U);
    chunk_count = read_be32(co64 + 12U);
    if (timescale == 0U || sample_count == 0U || chunk_count != sample_count ||
        read_be32(stsz + 12U) != 0U ||
        stts_size != 16U + (size_t)timing_entries * 8U ||
        stsz_size != 20U + (size_t)sample_count * 4U ||
        co64_size != 16U + (size_t)chunk_count * 8U) {
        fprintf(stderr, "inconsistent table sizes/counts\n");
        goto done;
    }
    for (index = 0U; index < timing_entries; ++index) {
        uint32_t count = read_be32(stts + 16U + (size_t)index * 8U);
        uint32_t duration = read_be32(stts + 20U + (size_t)index * 8U);

        if (count == 0U || duration == 0U) {
            fprintf(stderr, "non-monotonic PTS entry=%u\n", index);
            goto done;
        }
        timing_sample_count += count;
        duration_sum += (uint64_t)count * duration;
    }
    if (timing_sample_count != sample_count || duration_sum != movie_duration) {
        fprintf(stderr, "duration/sample count mismatch\n");
        goto done;
    }
    for (index = 0U; index < sample_count; ++index) {
        uint32_t sample_size = read_be32(stsz + 20U + (size_t)index * 4U);
        uint64_t sample_offset = read_be64(co64 + 16U + (size_t)index * 8U);
        uint64_t mdat_begin = mdat.offset + mdat.header_size;
        uint64_t mdat_end = mdat.offset + mdat.size;

        if (sample_size == 0U || sample_offset < mdat_begin ||
            sample_offset > mdat_end || sample_size > mdat_end - sample_offset ||
            !sample_has_jpeg_markers(file, sample_offset, sample_size)) {
            fprintf(stderr, "invalid JPEG sample=%u offset=%" PRIu64
                            " size=%u\n", index, sample_offset, sample_size);
            goto done;
        }
        if (index + 1U < sample_count) {
            uint64_t next_offset = read_be64(
                co64 + 16U + (size_t)(index + 1U) * 8U);

            if (next_offset <= sample_offset ||
                sample_size > next_offset - sample_offset) {
                fprintf(stderr, "overlapping/non-monotonic sample=%u\n", index);
                goto done;
            }
        }
    }
    printf("file=%s bytes=%" PRIu64 " frames=%u duration=%.6fs fps=%.6f "
           "timescale=%u pts_monotonic=yes jpeg_markers=%u/%u tail=moov\n",
           argv[1], file_size, sample_count,
           (double)duration_sum / (double)timescale,
           (double)sample_count * (double)timescale / (double)duration_sum,
           timescale, sample_count, sample_count);
    printf("samples first[offset=%" PRIu64 " size=%u] "
           "middle[offset=%" PRIu64 " size=%u] "
           "last[offset=%" PRIu64 " size=%u]\n",
           read_be64(co64 + 16U), read_be32(stsz + 20U),
           read_be64(co64 + 16U + (size_t)(sample_count / 2U) * 8U),
           read_be32(stsz + 20U + (size_t)(sample_count / 2U) * 4U),
           read_be64(co64 + 16U + (size_t)(sample_count - 1U) * 8U),
           read_be32(stsz + 20U + (size_t)(sample_count - 1U) * 4U));
    result = EXIT_SUCCESS;
done:
    free(movie);
    fclose(file);
    return result;
}
