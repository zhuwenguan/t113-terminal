#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include "video_recorder.h"
#include "console_log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef VideoRecorderPendingSample VideoRecorderSample;

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
} Mp4Buffer;

static uint64_t video_recorder_now_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return 0U;
    }
    return (uint64_t)now.tv_sec * 1000000ULL +
           (uint64_t)now.tv_nsec / 1000ULL;
}

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
    uint64_t duration = 0U;
    uint32_t timing_entries = 0U;
    uint64_t index;

    for (index = 0U; index < recorder->frame_count; ++index) {
        duration += samples[index].duration;
        if (index == 0U || samples[index].duration !=
                           samples[index - 1U].duration) {
            timing_entries++;
        }
    }

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
        !mp4_append_u32(buffer, timing_entries)) {
        return false;
    }
    index = 0U;
    while (index < recorder->frame_count) {
        uint64_t run_end = index + 1U;

        while (run_end < recorder->frame_count &&
               samples[run_end].duration == samples[index].duration) {
            run_end++;
        }
        if (!mp4_append_u32(buffer, (uint32_t)(run_end - index)) ||
            !mp4_append_u32(buffer, samples[index].duration)) {
            return false;
        }
        index = run_end;
    }
    if (!mp4_end(buffer, stts)) {
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

static bool video_recorder_write_u64_at(FILE *file, uint64_t offset,
                                        uint64_t value)
{
    uint8_t bytes[8] = {
        (uint8_t)(value >> 56), (uint8_t)(value >> 48),
        (uint8_t)(value >> 40), (uint8_t)(value >> 32),
        (uint8_t)(value >> 24), (uint8_t)(value >> 16),
        (uint8_t)(value >> 8), (uint8_t)value
    };

    if (fseeko(file, (off_t)offset, SEEK_SET) != 0 ||
        fwrite(bytes, 1U, sizeof(bytes), file) != sizeof(bytes)) {
        return false;
    }
    return true;
}

static void video_recorder_close_mux_files(VideoRecorder *recorder)
{
    if (recorder->index_file != NULL) {
        fclose(recorder->index_file);
        recorder->index_file = NULL;
        (void)unlink(recorder->index_path);
    }
    if (recorder->file != NULL) {
        fclose(recorder->file);
        recorder->file = NULL;
    }
}

static int video_recorder_mux_open(VideoRecorder *recorder)
{
    uint8_t mdat_header[16] = {
        0U, 0U, 0U, 1U, 'm','d','a','t',
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U
    };
    int file_fd = -1;
    int index_fd = -1;
    int saved_errno;

    file_fd = open(recorder->path,
                   O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0664);
    if (file_fd < 0) {
        return -1;
    }
    recorder->file = fdopen(file_fd, "wb+");
    if (recorder->file == NULL) {
        saved_errno = errno;
        close(file_fd);
        (void)unlink(recorder->path);
        errno = saved_errno;
        return -1;
    }
    file_fd = -1;
    index_fd = open(recorder->index_path,
                    O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0664);
    if (index_fd >= 0) {
        recorder->index_file = fdopen(index_fd, "wb+");
        if (recorder->index_file != NULL) {
            index_fd = -1;
        }
    }
    if (recorder->index_file == NULL ||
        !video_recorder_write_ftyp(recorder->file) ||
        fwrite(mdat_header, 1U, sizeof(mdat_header), recorder->file) !=
            sizeof(mdat_header)) {
        saved_errno = errno != 0 ? errno : EIO;
        if (index_fd >= 0) {
            close(index_fd);
            (void)unlink(recorder->index_path);
        }
        video_recorder_close_mux_files(recorder);
        (void)unlink(recorder->path);
        errno = saved_errno;
        return -1;
    }
    recorder->mdat_size_offset = 32U;
    return 0;
}

static bool video_recorder_commit_pending_sample(VideoRecorder *recorder,
                                                 uint64_t next_timestamp_us,
                                                 uint64_t *write_calls,
                                                 uint64_t *write_bytes)
{
    uint64_t delta_us;
    uint64_t duration;

    if (!recorder->have_pending_sample) {
        return true;
    }
    delta_us = next_timestamp_us > recorder->pending_timestamp_us ?
               next_timestamp_us - recorder->pending_timestamp_us :
               1000000ULL / recorder->fps;
    duration = (delta_us * 90000ULL + 500000ULL) / 1000000ULL;
    if (duration == 0U) {
        duration = 1U;
    } else if (duration > UINT32_MAX) {
        duration = UINT32_MAX;
    }
    recorder->pending_sample.duration = (uint32_t)duration;
    if (write_calls != NULL) {
        (*write_calls)++;
    }
    if (write_bytes != NULL) {
        *write_bytes += sizeof(recorder->pending_sample);
    }
    if (fwrite(&recorder->pending_sample, 1U,
               sizeof(recorder->pending_sample), recorder->index_file) !=
        sizeof(recorder->pending_sample)) {
        return false;
    }
    recorder->have_pending_sample = false;
    return true;
}

static bool video_recorder_mux_write_encoded(VideoRecorder *recorder,
                                             const EncodedFrame *frame,
                                             uint64_t *write_calls,
                                             uint64_t *write_bytes)
{
    off_t offset;
    VideoRecorderSample sample;

    if (frame == NULL || frame->data == NULL || frame->size == 0U ||
        frame->size > UINT32_MAX || recorder->file == NULL ||
        recorder->index_file == NULL) {
        return false;
    }
    offset = ftello(recorder->file);
    if (write_calls != NULL) {
        (*write_calls)++;
    }
    if (write_bytes != NULL) {
        *write_bytes += frame->size;
    }
    if (offset < 0 || fwrite(frame->data, 1U, frame->size, recorder->file) !=
                          frame->size) {
        return false;
    }
    sample.offset = (uint64_t)offset;
    sample.size = (uint32_t)frame->size;
    sample.duration = 0U;
    if (!video_recorder_commit_pending_sample(recorder, frame->pts_us,
                                               write_calls, write_bytes)) {
        return false;
    }
    recorder->pending_sample = sample;
    recorder->pending_timestamp_us = frame->pts_us;
    recorder->have_pending_sample = true;
    recorder->media_bytes += frame->size;
    recorder->frame_count++;
    return true;
}

static int video_recorder_mux_finalize(VideoRecorder *recorder)
{
    VideoRecorderSample *samples = NULL;
    Mp4Buffer movie = { 0 };
    off_t index_size;
    uint64_t sample_count;
    int result = -1;

    if (recorder->file == NULL || recorder->index_file == NULL) {
        return -1;
    }
    if (recorder->have_pending_sample) {
        uint64_t nominal_next = recorder->pending_timestamp_us +
                                1000000ULL / recorder->fps;
        if (!video_recorder_commit_pending_sample(recorder, nominal_next,
                                                   NULL, NULL)) {
            goto done;
        }
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
    if (recorder->media_bytes > UINT64_MAX - 16U ||
        !video_recorder_write_u64_at(recorder->file, recorder->mdat_size_offset,
                                     recorder->media_bytes + 16U) ||
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
    video_recorder_close_mux_files(recorder);
    return result;
}

static void video_recorder_mark_writer_failed(VideoRecorder *recorder)
{
    if (pthread_mutex_lock(&recorder->state_mutex) == 0) {
        recorder->writer_failed = true;
        recorder->stats.writer_failed = true;
        recorder->stats.recording_failed = true;
        recorder->stats.active = false;
        recorder->stats.writer_errors++;
        pthread_mutex_unlock(&recorder->state_mutex);
    }
    encoded_frame_queue_close(&recorder->writer_queue);
}

static void video_recorder_record_write_latency(VideoRecorderStats *stats,
                                                uint64_t elapsed_us)
{
    uint64_t bucket;

    stats->write_latency.samples++;
    stats->write_latency.total_us += elapsed_us;
    if (elapsed_us > stats->write_latency.max_us) {
        stats->write_latency.max_us = elapsed_us;
    }
    bucket = elapsed_us / VIDEO_WRITER_LATENCY_BUCKET_US;
    if (bucket >= VIDEO_WRITER_LATENCY_BUCKET_COUNT) {
        bucket = VIDEO_WRITER_LATENCY_BUCKET_COUNT - 1U;
    }
    stats->write_latency.buckets[bucket]++;
    if (elapsed_us > 50000U) {
        stats->writes_over_50ms++;
    }
    if (elapsed_us > 100000U) {
        stats->writes_over_100ms++;
    }
    if (elapsed_us > 200000U) {
        stats->writes_over_200ms++;
    }
    if (elapsed_us > 500000U) {
        stats->writes_over_500ms++;
    }
}

static void *video_recorder_writer_thread(void *opaque)
{
    VideoRecorder *recorder = opaque;
    EncodedFrame frame;
    bool failed = video_recorder_mux_open(recorder) < 0;

    if (pthread_mutex_lock(&recorder->state_mutex) == 0) {
        recorder->writer_ready = true;
        if (failed) {
            recorder->writer_failed = true;
            recorder->stats.writer_failed = true;
            recorder->stats.recording_failed = true;
            recorder->stats.writer_errors++;
            recorder->stats.active = false;
        } else {
            recorder->stats.active = true;
        }
        pthread_cond_broadcast(&recorder->state_condition);
        pthread_mutex_unlock(&recorder->state_mutex);
    }
    if (failed) {
        encoded_frame_queue_close(&recorder->writer_queue);
        recorder->writer_result = -1;
        return NULL;
    }

    memset(&frame, 0, sizeof(frame));
    while (encoded_frame_queue_pop(&recorder->writer_queue, &frame)) {
        uint64_t write_calls = 0U;
        uint64_t write_bytes = 0U;
        uint64_t write_start_us = video_recorder_now_us();
        bool written = !failed && video_recorder_mux_write_encoded(
            recorder, &frame, &write_calls, &write_bytes);
        uint64_t write_elapsed_us = video_recorder_now_us() - write_start_us;

        if (pthread_mutex_lock(&recorder->state_mutex) == 0) {
            if (!failed) {
                video_recorder_record_write_latency(&recorder->stats,
                                                    write_elapsed_us);
                recorder->stats.writer_write_calls += write_calls;
                recorder->stats.writer_write_call_bytes += write_bytes;
            }
            if (written) {
                recorder->stats.written_frames++;
                recorder->stats.bytes_written += frame.size;
            } else {
                recorder->stats.dropped_frames++;
            }
            pthread_mutex_unlock(&recorder->state_mutex);
        }
        if (!failed && !written) {
            failed = true;
            video_recorder_mark_writer_failed(recorder);
        }
        encoded_frame_queue_release(&recorder->writer_queue, &frame);
    }
    if (video_recorder_mux_finalize(recorder) < 0) {
        if (!failed) {
            video_recorder_mark_writer_failed(recorder);
        }
        failed = true;
    }
    if (pthread_mutex_lock(&recorder->state_mutex) == 0) {
        recorder->stats.active = false;
        pthread_mutex_unlock(&recorder->state_mutex);
    }
    recorder->writer_result = failed ? -1 : 0;
    return NULL;
}

static void video_recorder_destroy_sync(VideoRecorder *recorder)
{
    encoded_frame_queue_destroy(&recorder->writer_queue);
    if (recorder->state_condition_initialized) {
        pthread_cond_destroy(&recorder->state_condition);
        recorder->state_condition_initialized = false;
    }
    if (recorder->state_mutex_initialized) {
        pthread_mutex_destroy(&recorder->state_mutex);
        recorder->state_mutex_initialized = false;
    }
}

static int video_recorder_init_format(VideoRecorder *recorder,
                                      const char *path, uint32_t width,
                                      uint32_t height, uint32_t fps,
                                      bool argb_hardware_input)
{
    bool hardware_encoder_ready = false;
    int result;

    if (recorder == NULL || path == NULL || path[0] == '\0' || width == 0U ||
        height == 0U || fps == 0U || strlen(path) >= sizeof(recorder->path) ||
        strlen(path) + 4U >= sizeof(recorder->index_path)) {
        errno = EINVAL;
        return -1;
    }
    memset(recorder, 0, sizeof(*recorder));
    (void)snprintf(recorder->path, sizeof(recorder->path), "%s", path);
    (void)snprintf(recorder->index_path, sizeof(recorder->index_path), "%s.idx",
                   path);
    recorder->width = width;
    recorder->height = height;
    recorder->fps = fps;
    recorder->argb_hardware_input = argb_hardware_input;
    result = pthread_mutex_init(&recorder->state_mutex, NULL);
    if (result != 0) {
        errno = result;
        return -1;
    }
    recorder->state_mutex_initialized = true;
    result = pthread_cond_init(&recorder->state_condition, NULL);
    if (result != 0) {
        errno = result;
        video_recorder_destroy_sync(recorder);
        return -1;
    }
    recorder->state_condition_initialized = true;
    if (encoded_frame_queue_init(&recorder->writer_queue,
                                 VIDEO_WRITER_QUEUE_CAPACITY,
                                 VIDEO_WRITER_SLOT_CAPACITY) < 0) {
        video_recorder_destroy_sync(recorder);
        return -1;
    }
    if ((argb_hardware_input ?
         video_encoder_init_argb(&recorder->hardware_encoder, width, height,
                                 fps, VIDEO_ENCODER_DEFAULT_JPEG_QUALITY,
                                 VIDEO_RECORDER_JPEG_CAPACITY) :
         video_encoder_init(&recorder->hardware_encoder, width, height, fps,
                            VIDEO_ENCODER_DEFAULT_JPEG_QUALITY,
                            VIDEO_RECORDER_JPEG_CAPACITY)) == 0) {
        hardware_encoder_ready = true;
    }
    if (!hardware_encoder_ready && argb_hardware_input) {
        errno = ENODEV;
        video_encoder_close(&recorder->hardware_encoder);
        video_recorder_destroy_sync(recorder);
        return -1;
    }
    if (!hardware_encoder_ready &&
        jpeg_encoder_init(&recorder->encoder, width,
                          VIDEO_RECORDER_JPEG_CAPACITY) < 0) {
        video_encoder_close(&recorder->hardware_encoder);
        video_recorder_destroy_sync(recorder);
        return -1;
    }
    recorder->hardware_encoder_ready = hardware_encoder_ready;
    recorder->software_encoder_ready = !hardware_encoder_ready;
    result = pthread_create(&recorder->writer_thread, NULL,
                            video_recorder_writer_thread, recorder);
    if (result != 0) {
        errno = result;
        jpeg_encoder_close(&recorder->encoder);
        video_encoder_close(&recorder->hardware_encoder);
        video_recorder_destroy_sync(recorder);
        return -1;
    }
    recorder->writer_thread_started = true;
    if (pthread_mutex_lock(&recorder->state_mutex) != 0) {
        encoded_frame_queue_close(&recorder->writer_queue);
        pthread_join(recorder->writer_thread, NULL);
        recorder->writer_joined = true;
        jpeg_encoder_close(&recorder->encoder);
        video_encoder_close(&recorder->hardware_encoder);
        video_recorder_destroy_sync(recorder);
        errno = EIO;
        return -1;
    }
    while (!recorder->writer_ready) {
        result = pthread_cond_wait(&recorder->state_condition,
                                   &recorder->state_mutex);
        if (result != 0) {
            break;
        }
    }
    pthread_mutex_unlock(&recorder->state_mutex);
    if (result != 0 || recorder->writer_failed) {
        encoded_frame_queue_close(&recorder->writer_queue);
        pthread_join(recorder->writer_thread, NULL);
        recorder->writer_joined = true;
        jpeg_encoder_close(&recorder->encoder);
        video_encoder_close(&recorder->hardware_encoder);
        video_recorder_destroy_sync(recorder);
        errno = result != 0 ? result : EIO;
        return -1;
    }
    recorder->initialized = true;
    LOG_INFO("Video recorder: Motion-JPEG MP4 output %s backend=%s input=%s "
             "writer_queue=%u slots x %uKB",
             recorder->path, hardware_encoder_ready ? "cedar-hardware" :
             "software", argb_hardware_input ? "ARGB8888" : "YUV420P",
             VIDEO_WRITER_QUEUE_CAPACITY,
             VIDEO_WRITER_SLOT_CAPACITY / 1024U);
    return 0;
}

int video_recorder_init(VideoRecorder *recorder, const char *path,
                        uint32_t width, uint32_t height, uint32_t fps)
{
    return video_recorder_init_format(recorder, path, width, height, fps,
                                      false);
}

int video_recorder_init_argb(VideoRecorder *recorder, const char *path,
                             uint32_t width, uint32_t height, uint32_t fps)
{
    return video_recorder_init_format(recorder, path, width, height, fps,
                                      true);
}

static void video_recorder_note_drop(VideoRecorder *recorder)
{
    if (pthread_mutex_lock(&recorder->state_mutex) == 0) {
        recorder->stats.dropped_frames++;
        pthread_mutex_unlock(&recorder->state_mutex);
    }
}

static bool video_recorder_begin_frame(VideoRecorder *recorder,
                                       uint64_t *frame_index)
{
    bool allowed = false;

    if (pthread_mutex_lock(&recorder->state_mutex) != 0) {
        return false;
    }
    if (recorder->initialized && recorder->stats.active &&
        !recorder->writer_failed && recorder->producer_frame_index <
        (uint64_t)VIDEO_RECORDER_MAX_DURATION_SECONDS * recorder->fps) {
        *frame_index = recorder->producer_frame_index++;
        allowed = true;
    } else {
        recorder->stats.dropped_frames++;
    }
    pthread_mutex_unlock(&recorder->state_mutex);
    return allowed;
}

static bool video_recorder_enqueue_encoded(
    VideoRecorder *recorder, const uint8_t *jpeg_data, size_t jpeg_size,
    uint64_t timestamp_us, uint32_t v4l2_sequence, uint64_t frame_index,
    VideoRecorderFrameTiming *timing)
{
    EncodedFrame frame;
    EncodedFrameQueuePushResult result;
    uint64_t enqueue_start_us = video_recorder_now_us();

    if (jpeg_data == NULL || jpeg_size == 0U || jpeg_size > UINT32_MAX) {
        video_recorder_note_drop(recorder);
        return false;
    }
    if (timing != NULL) {
        timing->encoded_size = jpeg_size;
        timing->encoded = true;
    }
    if (pthread_mutex_lock(&recorder->state_mutex) == 0) {
        recorder->stats.encoded_frames++;
        recorder->stats.encoded_bytes += jpeg_size;
        pthread_mutex_unlock(&recorder->state_mutex);
    }
    memset(&frame, 0, sizeof(frame));
    frame.data = (uint8_t *)jpeg_data;
    frame.size = jpeg_size;
    frame.pts_us = timestamp_us;
    frame.capture_ts_us = timestamp_us;
    frame.v4l2_sequence = v4l2_sequence;
    frame.frame_index = frame_index;
    frame.keyframe = true;
    result = encoded_frame_queue_try_push(&recorder->writer_queue, &frame);
    if (timing != NULL) {
        timing->enqueue_copy_time_us =
            video_recorder_now_us() - enqueue_start_us;
        timing->enqueued = result == ENCODED_FRAME_QUEUE_PUSH_OK;
    }
    if (result == ENCODED_FRAME_QUEUE_PUSH_OK) {
        if (pthread_mutex_lock(&recorder->state_mutex) == 0) {
            recorder->stats.enqueued_frames++;
            recorder->stats.enqueued_bytes += jpeg_size;
            pthread_mutex_unlock(&recorder->state_mutex);
        }
        return true;
    }
    video_recorder_note_drop(recorder);
    if (result == ENCODED_FRAME_QUEUE_PUSH_TOO_LARGE) {
        LOG_WARN("Video writer queue: JPEG frame too large (%zu > %u bytes); "
                 "dropping frame", jpeg_size, VIDEO_WRITER_SLOT_CAPACITY);
    }
    return false;
}

bool video_recorder_write_frame_timed(VideoRecorder *recorder,
                                      const VideoFrame *frame,
                                      uint32_t v4l2_sequence,
                                      VideoRecorderFrameTiming *timing)
{
    const uint8_t *jpeg_data;
    size_t jpeg_size;
    uint64_t total_start_us;
    uint64_t encode_start_us;
    uint64_t frame_index;
    bool encoded;
    bool success = false;

    if (timing != NULL) {
        memset(timing, 0, sizeof(*timing));
    }
    if (recorder == NULL || frame == NULL ||
        !video_recorder_begin_frame(recorder, &frame_index)) {
        return false;
    }
    total_start_us = video_recorder_now_us();
    encode_start_us = video_recorder_now_us();
    encoded = jpeg_encoder_encode_rgba(&recorder->encoder, frame, 82,
                                       &jpeg_data, &jpeg_size);
    if (timing != NULL) {
        timing->encode_time_us = video_recorder_now_us() - encode_start_us;
    }
    if (!encoded || jpeg_size > UINT32_MAX) {
        video_recorder_note_drop(recorder);
        goto done;
    }
    if (!video_recorder_enqueue_encoded(
            recorder, jpeg_data, jpeg_size,
            frame->timestamp_us != 0U ? frame->timestamp_us :
            video_recorder_now_us(), v4l2_sequence, frame_index, timing)) {
        goto done;
    }
    success = true;
done:
    if (timing != NULL) {
        if (timing->total_time_us == 0U) {
            timing->total_time_us = video_recorder_now_us() - total_start_us;
        }
    }
    return success;
}

bool video_recorder_write_native_frame_timed(VideoRecorder *recorder,
                                             const VideoYuvFrame *frame,
                                             uint64_t timestamp_us,
                                             uint32_t v4l2_sequence,
                                             VideoRecorderFrameTiming *timing)
{
    const uint8_t *jpeg_data = NULL;
    size_t jpeg_size = 0U;
    VideoEncoderTiming encoder_timing;
    uint64_t total_start_us;
    uint64_t frame_index;
    bool success;

    if (timing != NULL) {
        memset(timing, 0, sizeof(*timing));
    }
    if (recorder == NULL || !recorder->hardware_encoder_ready || frame == NULL ||
        !video_recorder_begin_frame(recorder, &frame_index)) {
        return false;
    }
    total_start_us = video_recorder_now_us();
    memset(&encoder_timing, 0, sizeof(encoder_timing));
    success = video_encoder_encode_yuv_timed(&recorder->hardware_encoder,
                                             frame, &jpeg_data, &jpeg_size,
                                             &encoder_timing);
    if (timing != NULL) {
        timing->encoder_submit_time_us = encoder_timing.submit_time_us;
        timing->encoder_input_acquire_time_us =
            encoder_timing.input_acquire_time_us;
        timing->encoder_input_copy_time_us = encoder_timing.input_copy_time_us;
        timing->encoder_cache_flush_time_us =
            encoder_timing.cache_flush_time_us;
        timing->encoder_input_prepare_time_us =
            encoder_timing.input_prepare_time_us;
        timing->encoder_handoff_time_us = encoder_timing.handoff_time_us;
        timing->encoder_hardware_time_us = encoder_timing.hardware_encode_time_us;
        timing->encoder_acquire_time_us = encoder_timing.acquire_time_us;
        timing->encoder_release_time_us = encoder_timing.release_time_us;
        timing->hardware_encoded = success;
    }
    if (!success) {
        recorder->hardware_encoder_ready = false;
        if (!recorder->software_encoder_ready &&
            jpeg_encoder_init(&recorder->encoder, recorder->width,
                              VIDEO_RECORDER_JPEG_CAPACITY) == 0) {
            recorder->software_encoder_ready = true;
        }
        video_recorder_note_drop(recorder);
        return false;
    }
    if (timing != NULL) {
        timing->encode_time_us = encoder_timing.total_time_us;
    }
    success = video_recorder_enqueue_encoded(
        recorder, jpeg_data, jpeg_size, timestamp_us, v4l2_sequence,
        frame_index, timing);
    if (timing != NULL) {
        timing->total_time_us = video_recorder_now_us() - total_start_us;
    }
    return success;
}


bool video_recorder_write_argb_frame_timed(VideoRecorder *recorder,
                                           const VideoFrame *frame,
                                           uint64_t timestamp_us,
                                           uint32_t v4l2_sequence,
                                           VideoRecorderFrameTiming *timing)
{
    const uint8_t *jpeg_data = NULL;
    size_t jpeg_size = 0U;
    VideoEncoderTiming encoder_timing;
    uint64_t total_start_us;
    uint64_t frame_index;
    bool success;

    if (timing != NULL) {
        memset(timing, 0, sizeof(*timing));
    }
    if (recorder == NULL || !recorder->hardware_encoder_ready ||
        !recorder->argb_hardware_input || frame == NULL ||
        !video_recorder_begin_frame(recorder, &frame_index)) {
        return false;
    }
    total_start_us = video_recorder_now_us();
    memset(&encoder_timing, 0, sizeof(encoder_timing));
    success = video_encoder_encode_argb_timed(&recorder->hardware_encoder,
                                              frame, &jpeg_data, &jpeg_size,
                                              &encoder_timing);
    if (timing != NULL) {
        timing->encoder_input_acquire_time_us =
            encoder_timing.input_acquire_time_us;
        timing->encoder_input_copy_time_us = encoder_timing.input_copy_time_us;
        timing->encoder_cache_flush_time_us =
            encoder_timing.cache_flush_time_us;
        timing->encoder_input_prepare_time_us =
            encoder_timing.input_prepare_time_us;
        timing->encoder_handoff_time_us = encoder_timing.handoff_time_us;
        timing->encoder_submit_time_us = encoder_timing.submit_time_us;
        timing->encoder_hardware_time_us =
            encoder_timing.hardware_encode_time_us;
        timing->encoder_acquire_time_us = encoder_timing.acquire_time_us;
        timing->encoder_release_time_us = encoder_timing.release_time_us;
        timing->hardware_encoded = success;
    }
    if (!success) {
        recorder->hardware_encoder_ready = false;
        video_recorder_note_drop(recorder);
        return false;
    }
    if (timing != NULL) {
        timing->encode_time_us = encoder_timing.total_time_us;
    }
    success = video_recorder_enqueue_encoded(
        recorder, jpeg_data, jpeg_size, timestamp_us, v4l2_sequence,
        frame_index, timing);
    if (timing != NULL) {
        timing->total_time_us = video_recorder_now_us() - total_start_us;
    }
    return success;
}

bool video_recorder_write_frame(VideoRecorder *recorder,
                                const VideoFrame *frame)
{
    return video_recorder_write_frame_timed(recorder, frame, 0U, NULL);
}

int video_recorder_finalize(VideoRecorder *recorder)
{
    bool joined_here = false;

    if (recorder == NULL || !recorder->writer_thread_started) {
        return 0;
    }
    if (!recorder->writer_joined) {
        encoded_frame_queue_close(&recorder->writer_queue);
        pthread_join(recorder->writer_thread, NULL);
        recorder->writer_joined = true;
        joined_here = true;
    }
    if (joined_here && recorder->writer_result < 0) {
        LOG_ERROR("Video recorder: failed to finalize %s", recorder->path);
    }
    return recorder->writer_result;
}

void video_recorder_close(VideoRecorder *recorder)
{
    if (recorder == NULL) {
        return;
    }
    (void)video_recorder_finalize(recorder);
    jpeg_encoder_close(&recorder->encoder);
    video_encoder_close(&recorder->hardware_encoder);
    video_recorder_destroy_sync(recorder);
    memset(recorder, 0, sizeof(*recorder));
}

bool video_recorder_supports_native_yuv(const VideoRecorder *recorder)
{
    return recorder != NULL && video_recorder_is_recording(recorder) &&
           recorder->hardware_encoder_ready &&
           !recorder->argb_hardware_input &&
           video_encoder_is_available(&recorder->hardware_encoder);
}

bool video_recorder_is_recording(const VideoRecorder *recorder)
{
    VideoRecorder *mutable_recorder = (VideoRecorder *)recorder;
    bool active = false;

    if (mutable_recorder != NULL && mutable_recorder->state_mutex_initialized &&
        pthread_mutex_lock(&mutable_recorder->state_mutex) == 0) {
        active = mutable_recorder->initialized &&
                 mutable_recorder->stats.active &&
                 !mutable_recorder->writer_failed;
        pthread_mutex_unlock(&mutable_recorder->state_mutex);
    }
    return active;
}

bool video_recorder_has_failed(const VideoRecorder *recorder)
{
    VideoRecorder *mutable_recorder = (VideoRecorder *)recorder;
    bool failed = false;

    if (mutable_recorder != NULL && mutable_recorder->state_mutex_initialized &&
        pthread_mutex_lock(&mutable_recorder->state_mutex) == 0) {
        failed = mutable_recorder->writer_failed ||
                 mutable_recorder->stats.recording_failed;
        pthread_mutex_unlock(&mutable_recorder->state_mutex);
    }
    return failed;
}

void video_recorder_get_stats(VideoRecorder *recorder,
                              VideoRecorderStats *stats)
{
    if (stats == NULL) {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    if (recorder != NULL && recorder->state_mutex_initialized &&
        pthread_mutex_lock(&recorder->state_mutex) == 0) {
        *stats = recorder->stats;
        pthread_mutex_unlock(&recorder->state_mutex);
    }
}

void video_recorder_get_queue_stats(VideoRecorder *recorder,
                                    EncodedFrameQueueStats *stats)
{
    encoded_frame_queue_get_stats(recorder != NULL ? &recorder->writer_queue :
                                  NULL, stats);
}
