#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include "video_recorder.h"
#include "console_log.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    uint64_t offset;
    uint32_t size;
    uint32_t duration;
} VideoRecorderSample;

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
} Mp4Buffer;

static void mp4_put_u16(Mp4Buffer *buffer, uint16_t value)
{
    uint8_t bytes[2] = { (uint8_t)(value >> 8), (uint8_t)value };
    memcpy(buffer->data + buffer->size, bytes, sizeof(bytes));
    buffer->size += sizeof(bytes);
}

static void mp4_put_u32(Mp4Buffer *buffer, uint32_t value)
{
    uint8_t bytes[4] = { (uint8_t)(value >> 24), (uint8_t)(value >> 16),
                         (uint8_t)(value >> 8), (uint8_t)value };
    memcpy(buffer->data + buffer->size, bytes, sizeof(bytes));
    buffer->size += sizeof(bytes);
}

static void mp4_put_u64(Mp4Buffer *buffer, uint64_t value)
{
    mp4_put_u32(buffer, (uint32_t)(value >> 32));
    mp4_put_u32(buffer, (uint32_t)value);
}

static void mp4_put_bytes(Mp4Buffer *buffer, const void *data, size_t size)
{
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
}

static bool mp4_reserve(Mp4Buffer *buffer, size_t additional)
{
    size_t needed;
    size_t capacity;
    uint8_t *data;

    if (buffer == NULL || additional > SIZE_MAX - buffer->size) {
        return false;
    }
    needed = buffer->size + additional;
    if (needed <= buffer->capacity) {
        return true;
    }
    capacity = buffer->capacity == 0U ? 4096U : buffer->capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2U) {
            capacity = needed;
            break;
        }
        capacity *= 2U;
    }
    data = realloc(buffer->data, capacity);
    if (data == NULL) {
        return false;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    return true;
}

static bool mp4_append_u16(Mp4Buffer *buffer, uint16_t value)
{
    if (!mp4_reserve(buffer, 2U)) {
        return false;
    }
    mp4_put_u16(buffer, value);
    return true;
}

static bool mp4_append_u32(Mp4Buffer *buffer, uint32_t value)
{
    if (!mp4_reserve(buffer, 4U)) {
        return false;
    }
    mp4_put_u32(buffer, value);
    return true;
}

static bool mp4_append_u64(Mp4Buffer *buffer, uint64_t value)
{
    if (!mp4_reserve(buffer, 8U)) {
        return false;
    }
    mp4_put_u64(buffer, value);
    return true;
}

static bool mp4_append_bytes(Mp4Buffer *buffer, const void *data, size_t size)
{
    if (!mp4_reserve(buffer, size)) {
        return false;
    }
    mp4_put_bytes(buffer, data, size);
    return true;
}

static size_t mp4_begin(Mp4Buffer *buffer, const char type[4])
{
    size_t position = buffer->size;

    if (!mp4_append_u32(buffer, 0U) || !mp4_append_bytes(buffer, type, 4U)) {
        return SIZE_MAX;
    }
    return position;
}

static bool mp4_end(Mp4Buffer *buffer, size_t position)
{
    uint32_t size;

    if (position == SIZE_MAX || buffer->size < position ||
        buffer->size - position > UINT32_MAX) {
        return false;
    }
    size = (uint32_t)(buffer->size - position);
    buffer->data[position] = (uint8_t)(size >> 24);
    buffer->data[position + 1U] = (uint8_t)(size >> 16);
    buffer->data[position + 2U] = (uint8_t)(size >> 8);
    buffer->data[position + 3U] = (uint8_t)size;
    return true;
}

static bool mp4_put_matrix(Mp4Buffer *buffer)
{
    static const uint32_t matrix[] = {
        0x00010000U, 0U, 0U, 0U, 0x00010000U, 0U,
        0U, 0U, 0x40000000U
    };
    size_t index;

    for (index = 0U; index < sizeof(matrix) / sizeof(matrix[0]); ++index) {
        if (!mp4_append_u32(buffer, matrix[index])) {
            return false;
        }
    }
    return true;
}

static bool mp4_build_movie(Mp4Buffer *buffer, const VideoRecorder *recorder,
                            const VideoRecorderSample *samples)
{
    size_t moov;
    size_t mvhd;
    size_t trak;
    size_t tkhd;
    size_t mdia;
    size_t mdhd;
    size_t hdlr;
    size_t minf;
    size_t dinf;
    size_t dref;
    size_t stbl;
    size_t stsd;
    size_t sample_entry;
    size_t stts;
    size_t stsc;
    size_t stsz;
    size_t co64;
    uint64_t duration = recorder->frame_count * 90000ULL / recorder->fps;
    uint32_t frame_duration = 90000U / recorder->fps;
    uint64_t index;

    moov = mp4_begin(buffer, "moov");
    mvhd = mp4_begin(buffer, "mvhd");
    if (mvhd == SIZE_MAX || !mp4_append_u32(buffer, 0U) ||
        !mp4_append_u32(buffer, 0U) || !mp4_append_u32(buffer, 0U) ||
        !mp4_append_u32(buffer, 90000U) ||
        !mp4_append_u32(buffer, (uint32_t)duration) ||
        !mp4_append_u32(buffer, 0x00010000U) ||
        !mp4_append_u16(buffer, 0x0100U) ||
        !mp4_append_u16(buffer, 0U) || !mp4_append_u32(buffer, 0U) ||
        !mp4_append_u32(buffer, 0U) || !mp4_put_matrix(buffer)) {
        return false;
    }
    for (index = 0U; index < 6U; ++index) {
        if (!mp4_append_u32(buffer, 0U)) {
            return false;
        }
    }
    if (!mp4_append_u32(buffer, 2U) || !mp4_end(buffer, mvhd)) {
        return false;
    }

    trak = mp4_begin(buffer, "trak");
    tkhd = mp4_begin(buffer, "tkhd");
    if (tkhd == SIZE_MAX || !mp4_append_u32(buffer, 7U) ||
        !mp4_append_u32(buffer, 0U) || !mp4_append_u32(buffer, 0U) ||
        !mp4_append_u32(buffer, 1U) || !mp4_append_u32(buffer, 0U) ||
        !mp4_append_u32(buffer, (uint32_t)duration) ||
        !mp4_append_u32(buffer, 0U) || !mp4_append_u32(buffer, 0U) ||
        !mp4_append_u16(buffer, 0U) || !mp4_append_u16(buffer, 0U) ||
        !mp4_append_u16(buffer, 0U) || !mp4_append_u16(buffer, 0U) ||
        !mp4_put_matrix(buffer) ||
        !mp4_append_u32(buffer, recorder->width << 16) ||
        !mp4_append_u32(buffer, recorder->height << 16) ||
        !mp4_end(buffer, tkhd)) {
        return false;
    }
    mdia = mp4_begin(buffer, "mdia");
    mdhd = mp4_begin(buffer, "mdhd");
    if (mdhd == SIZE_MAX || !mp4_append_u32(buffer, 0U) ||
        !mp4_append_u32(buffer, 0U) || !mp4_append_u32(buffer, 0U) ||
        !mp4_append_u32(buffer, 90000U) ||
        !mp4_append_u32(buffer, (uint32_t)duration) ||
        !mp4_append_u16(buffer, 0x55c4U) || !mp4_append_u16(buffer, 0U) ||
        !mp4_end(buffer, mdhd)) {
        return false;
    }
    hdlr = mp4_begin(buffer, "hdlr");
    if (hdlr == SIZE_MAX || !mp4_append_u32(buffer, 0U) ||
        !mp4_append_u32(buffer, 0U) || !mp4_append_bytes(buffer, "vide", 4U) ||
        !mp4_append_u32(buffer, 0U) || !mp4_append_u32(buffer, 0U) ||
        !mp4_append_u32(buffer, 0U) ||
        !mp4_append_bytes(buffer, "T113 MJPEG\0", 11U) ||
        !mp4_end(buffer, hdlr)) {
        return false;
    }
    minf = mp4_begin(buffer, "minf");
    {
        size_t vmhd = mp4_begin(buffer, "vmhd");
        if (vmhd == SIZE_MAX || !mp4_append_u32(buffer, 1U) ||
            !mp4_append_u16(buffer, 0U) || !mp4_append_u16(buffer, 0U) ||
            !mp4_append_u16(buffer, 0U) || !mp4_end(buffer, vmhd)) {
            return false;
        }
    }
    dinf = mp4_begin(buffer, "dinf");
    dref = mp4_begin(buffer, "dref");
    if (dref == SIZE_MAX || !mp4_append_u32(buffer, 0U) ||
        !mp4_append_u32(buffer, 1U)) {
        return false;
    }
    {
        size_t url = mp4_begin(buffer, "url ");
        if (url == SIZE_MAX || !mp4_append_u32(buffer, 1U) ||
            !mp4_end(buffer, url)) {
            return false;
        }
    }
    if (!mp4_end(buffer, dref) || !mp4_end(buffer, dinf)) {
        return false;
    }
    stbl = mp4_begin(buffer, "stbl");
    stsd = mp4_begin(buffer, "stsd");
    if (stsd == SIZE_MAX || !mp4_append_u32(buffer, 0U) ||
        !mp4_append_u32(buffer, 1U)) {
        return false;
    }
    sample_entry = mp4_begin(buffer, "mjpg");
    if (sample_entry == SIZE_MAX || !mp4_append_u32(buffer, 0U) ||
        !mp4_append_u16(buffer, 1U) || !mp4_append_u16(buffer, 0U) ||
        !mp4_append_u16(buffer, 0U) || !mp4_append_u32(buffer, 0U) ||
        !mp4_append_u32(buffer, 0U) || !mp4_append_u32(buffer, 0U) ||
        !mp4_append_u16(buffer, (uint16_t)recorder->width) ||
        !mp4_append_u16(buffer, (uint16_t)recorder->height) ||
        !mp4_append_u32(buffer, 0x00480000U) ||
        !mp4_append_u32(buffer, 0x00480000U) || !mp4_append_u32(buffer, 0U) ||
        !mp4_append_u16(buffer, 1U)) {
        return false;
    }
    {
        uint8_t compressor_name[32] = { 0U };
        memcpy(compressor_name + 1U, "T113 MJPEG", 10U);
        compressor_name[0] = 10U;
        if (!mp4_append_bytes(buffer, compressor_name, sizeof(compressor_name)) ||
            !mp4_append_u16(buffer, 0x0018U) ||
            !mp4_append_u16(buffer, 0xffffU) || !mp4_end(buffer, sample_entry)) {
            return false;
        }
    }
    if (!mp4_end(buffer, stsd)) {
        return false;
    }
    stts = mp4_begin(buffer, "stts");
    if (stts == SIZE_MAX || !mp4_append_u32(buffer, 0U) ||
        !mp4_append_u32(buffer, 1U) ||
        !mp4_append_u32(buffer, (uint32_t)recorder->frame_count) ||
        !mp4_append_u32(buffer, frame_duration) || !mp4_end(buffer, stts)) {
        return false;
    }
    stsc = mp4_begin(buffer, "stsc");
    if (stsc == SIZE_MAX || !mp4_append_u32(buffer, 0U) ||
        !mp4_append_u32(buffer, 1U) || !mp4_append_u32(buffer, 1U) ||
        !mp4_append_u32(buffer, 1U) || !mp4_append_u32(buffer, 1U) ||
        !mp4_end(buffer, stsc)) {
        return false;
    }
    stsz = mp4_begin(buffer, "stsz");
    if (stsz == SIZE_MAX || !mp4_append_u32(buffer, 0U) ||
        !mp4_append_u32(buffer, 0U) ||
        !mp4_append_u32(buffer, (uint32_t)recorder->frame_count)) {
        return false;
    }
    for (index = 0U; index < recorder->frame_count; ++index) {
        if (!mp4_append_u32(buffer, samples[index].size)) {
            return false;
        }
    }
    if (!mp4_end(buffer, stsz)) {
        return false;
    }
    co64 = mp4_begin(buffer, "co64");
    if (co64 == SIZE_MAX || !mp4_append_u32(buffer, 0U) ||
        !mp4_append_u32(buffer, (uint32_t)recorder->frame_count)) {
        return false;
    }
    for (index = 0U; index < recorder->frame_count; ++index) {
        if (!mp4_append_u64(buffer, samples[index].offset)) {
            return false;
        }
    }
    if (!mp4_end(buffer, co64) || !mp4_end(buffer, stbl) ||
        !mp4_end(buffer, minf) || !mp4_end(buffer, mdia) ||
        !mp4_end(buffer, trak) || !mp4_end(buffer, moov)) {
        return false;
    }
    return true;
}

static bool video_recorder_write_ftyp(FILE *file)
{
    static const uint8_t ftyp[] = {
        0x00,0x00,0x00,0x18, 'f','t','y','p',
        'i','s','o','m', 0x00,0x00,0x02,0x00,
        'i','s','o','m', 'm','j','p','g'
    };
    return fwrite(ftyp, 1U, sizeof(ftyp), file) == sizeof(ftyp);
}

static bool video_recorder_write_u32_at(FILE *file, uint64_t offset,
                                         uint32_t value)
{
    uint8_t bytes[4] = { (uint8_t)(value >> 24), (uint8_t)(value >> 16),
                         (uint8_t)(value >> 8), (uint8_t)value };

    if (fseeko(file, (off_t)offset, SEEK_SET) != 0 ||
        fwrite(bytes, 1U, sizeof(bytes), file) != sizeof(bytes)) {
        return false;
    }
    return true;
}

int video_recorder_init(VideoRecorder *recorder, const char *path,
                        uint32_t width, uint32_t height, uint32_t fps)
{
    uint8_t mdat_header[8] = { 0U, 0U, 0U, 0U, 'm','d','a','t' };

    if (recorder == NULL || path == NULL || path[0] == '\0' || width == 0U ||
        height == 0U || fps == 0U || strlen(path) >= sizeof(recorder->path) ||
        strlen(path) + 4U >= sizeof(recorder->index_path)) {
        errno = EINVAL;
        return -1;
    }
    memset(recorder, 0, sizeof(*recorder));
    recorder->file = fopen(path, "wb+");
    if (recorder->file == NULL) {
        return -1;
    }
    recorder->index_file = NULL;
    (void)snprintf(recorder->path, sizeof(recorder->path), "%s", path);
    (void)snprintf(recorder->index_path, sizeof(recorder->index_path), "%s.idx",
                   path);
    recorder->index_file = fopen(recorder->index_path, "wb+");
    if (recorder->index_file == NULL || !video_recorder_write_ftyp(recorder->file) ||
        fwrite(mdat_header, 1U, sizeof(mdat_header), recorder->file) !=
            sizeof(mdat_header) ||
        jpeg_encoder_init(&recorder->encoder, width,
                          VIDEO_RECORDER_JPEG_CAPACITY) < 0) {
        video_recorder_close(recorder);
        return -1;
    }
    recorder->mdat_size_offset = 24U;
    recorder->width = width;
    recorder->height = height;
    recorder->fps = fps;
    recorder->initialized = true;
    recorder->stats.active = true;
    LOG_INFO("Video recorder: Motion-JPEG MP4 output %s", recorder->path);
    return 0;
}

bool video_recorder_write_frame(VideoRecorder *recorder,
                                const VideoFrame *frame)
{
    const uint8_t *jpeg_data;
    size_t jpeg_size;
    off_t offset;
    VideoRecorderSample sample;

    if (recorder == NULL || !recorder->initialized || recorder->file == NULL ||
        recorder->index_file == NULL || frame == NULL) {
        return false;
    }
    if (recorder->frame_count >=
        (uint64_t)VIDEO_RECORDER_MAX_DURATION_SECONDS * recorder->fps ||
        !jpeg_encoder_encode_rgba(&recorder->encoder, frame, 82, &jpeg_data,
                                  &jpeg_size) || jpeg_size > UINT32_MAX) {
        recorder->stats.dropped_frames++;
        return false;
    }
    offset = ftello(recorder->file);
    if (offset < 0 || fwrite(jpeg_data, 1U, jpeg_size, recorder->file) !=
                          jpeg_size) {
        recorder->stats.dropped_frames++;
        return false;
    }
    sample.offset = (uint64_t)offset;
    sample.size = (uint32_t)jpeg_size;
    sample.duration = 90000U / recorder->fps;
    if (fwrite(&sample, 1U, sizeof(sample), recorder->index_file) !=
        sizeof(sample)) {
        recorder->stats.dropped_frames++;
        return false;
    }
    recorder->media_bytes += jpeg_size;
    recorder->frame_count++;
    recorder->stats.encoded_frames++;
    recorder->stats.bytes_written += jpeg_size;
    return true;
}

int video_recorder_finalize(VideoRecorder *recorder)
{
    VideoRecorderSample *samples = NULL;
    Mp4Buffer movie = { 0 };
    off_t index_size;
    uint64_t sample_count;
    int result = -1;

    if (recorder == NULL || !recorder->initialized) {
        return 0;
    }
    if (fflush(recorder->index_file) != 0 ||
        fseeko(recorder->index_file, 0, SEEK_END) != 0 ||
        (index_size = ftello(recorder->index_file)) < 0 ||
        (uint64_t)index_size % sizeof(VideoRecorderSample) != 0U) {
        goto done;
    }
    sample_count = (uint64_t)index_size / sizeof(VideoRecorderSample);
    if (sample_count != recorder->frame_count || sample_count > SIZE_MAX /
        sizeof(VideoRecorderSample)) {
        goto done;
    }
    if (sample_count > 0U) {
        samples = malloc((size_t)sample_count * sizeof(*samples));
        if (samples == NULL || fseeko(recorder->index_file, 0, SEEK_SET) != 0 ||
            fread(samples, sizeof(*samples), (size_t)sample_count,
                  recorder->index_file) != (size_t)sample_count) {
            goto done;
        }
    }
    if (recorder->media_bytes > UINT32_MAX - 8U ||
        !video_recorder_write_u32_at(recorder->file, recorder->mdat_size_offset,
                                     (uint32_t)(recorder->media_bytes + 8U)) ||
        fseeko(recorder->file, 0, SEEK_END) != 0 ||
        !mp4_build_movie(&movie, recorder, samples) ||
        fwrite(movie.data, 1U, movie.size, recorder->file) != movie.size ||
        fflush(recorder->file) != 0) {
        goto done;
    }
    result = 0;
done:
    free(movie.data);
    free(samples);
    if (recorder->index_file != NULL) {
        fclose(recorder->index_file);
        recorder->index_file = NULL;
        (void)unlink(recorder->index_path);
    }
    if (recorder->file != NULL) {
        fclose(recorder->file);
        recorder->file = NULL;
    }
    recorder->initialized = false;
    recorder->stats.active = false;
    if (result < 0) {
        LOG_ERROR("Video recorder: failed to finalize %s", recorder->path);
    }
    return result;
}

void video_recorder_close(VideoRecorder *recorder)
{
    if (recorder == NULL) {
        return;
    }
    if (recorder->initialized) {
        (void)video_recorder_finalize(recorder);
    } else {
        if (recorder->index_file != NULL) {
            fclose(recorder->index_file);
        }
        if (recorder->file != NULL) {
            fclose(recorder->file);
        }
    }
    jpeg_encoder_close(&recorder->encoder);
    memset(recorder, 0, sizeof(*recorder));
}

bool video_recorder_is_recording(const VideoRecorder *recorder)
{
    return recorder != NULL && recorder->initialized &&
           recorder->stats.active;
}

const VideoRecorderStats *video_recorder_stats(const VideoRecorder *recorder)
{
    return recorder != NULL ? &recorder->stats : NULL;
}
