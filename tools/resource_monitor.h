#ifndef T113_RESOURCE_MONITOR_H
#define T113_RESOURCE_MONITOR_H

/*
 * Low-frequency resource diagnostics for the board-side accelerator tests.
 * This is intentionally header-only: the standalone tests are deployed as
 * independent binaries and do not need another shared library.
 */

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RESOURCE_MONITOR_ION_SUMMARY_SIZE 512U

typedef struct {
    bool valid;
    uint64_t frame;
    long rss_kb;
    int fd_count;
    bool have_mem_free;
    bool have_mem_available;
    bool have_cma_total;
    bool have_cma_free;
    bool have_slab;
    bool have_sreclaimable;
    bool have_sunreclaim;
    uint64_t mem_free_kb;
    uint64_t mem_available_kb;
    uint64_t cma_total_kb;
    uint64_t cma_free_kb;
    uint64_t slab_kb;
    uint64_t sreclaimable_kb;
    uint64_t sunreclaim_kb;
    bool have_dma_buf;
    uint64_t dma_buf_objects;
    uint64_t dma_buf_bytes;
    char ion_summary[RESOURCE_MONITOR_ION_SUMMARY_SIZE];
} ResourceSample;

typedef struct {
    unsigned int interval;
    bool have_first;
    bool have_last;
    ResourceSample first;
    ResourceSample last;
} ResourceMonitor;

static bool resource_monitor_parse_value(const char *line, const char *key,
                                         uint64_t *value)
{
    const char *cursor;
    char *end;
    unsigned long long parsed;

    if (line == NULL || key == NULL || value == NULL ||
        strncmp(line, key, strlen(key)) != 0) {
        return false;
    }
    cursor = line + strlen(key);
    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }
    errno = 0;
    parsed = strtoull(cursor, &end, 10);
    if (errno != 0 || end == cursor) {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static bool resource_monitor_read_meminfo(ResourceSample *sample)
{
    FILE *file;
    char line[256];
    uint64_t value;

    file = fopen("/proc/meminfo", "r");
    if (file == NULL) {
        return false;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        if (resource_monitor_parse_value(line, "MemFree:", &value)) {
            sample->mem_free_kb = value;
            sample->have_mem_free = true;
        } else if (resource_monitor_parse_value(line, "MemAvailable:",
                                                &value)) {
            sample->mem_available_kb = value;
            sample->have_mem_available = true;
        } else if (resource_monitor_parse_value(line, "CmaTotal:", &value)) {
            sample->cma_total_kb = value;
            sample->have_cma_total = true;
        } else if (resource_monitor_parse_value(line, "CmaFree:", &value)) {
            sample->cma_free_kb = value;
            sample->have_cma_free = true;
        } else if (resource_monitor_parse_value(line, "Slab:", &value)) {
            sample->slab_kb = value;
            sample->have_slab = true;
        } else if (resource_monitor_parse_value(line, "SReclaimable:",
                                                &value)) {
            sample->sreclaimable_kb = value;
            sample->have_sreclaimable = true;
        } else if (resource_monitor_parse_value(line, "SUnreclaim:",
                                                &value)) {
            sample->sunreclaim_kb = value;
            sample->have_sunreclaim = true;
        }
    }
    fclose(file);
    return true;
}

static bool resource_monitor_read_rss(ResourceSample *sample)
{
    FILE *file;
    char line[256];
    uint64_t value;

    file = fopen("/proc/self/status", "r");
    if (file == NULL) {
        return false;
    }
    sample->rss_kb = -1L;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (resource_monitor_parse_value(line, "VmRSS:", &value)) {
            sample->rss_kb = (long)value;
            break;
        }
    }
    fclose(file);
    return sample->rss_kb >= 0L;
}

static int resource_monitor_count_fds(void)
{
    DIR *directory;
    struct dirent *entry;
    int count = 0;

    directory = opendir("/proc/self/fd");
    if (directory == NULL) {
        return -1;
    }
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 &&
            strcmp(entry->d_name, "..") != 0) {
            ++count;
        }
    }
    /* The directory handle itself is included in the enumeration. */
    closedir(directory);
    return count > 0 ? count - 1 : 0;
}

static bool resource_monitor_read_dma_buf(ResourceSample *sample)
{
    FILE *file;
    char line[256];
    uint64_t objects;
    uint64_t bytes;

    file = fopen("/sys/kernel/debug/dma_buf/bufinfo", "r");
    if (file == NULL) {
        return false;
    }
    sample->have_dma_buf = true;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (sscanf(line, "Total %" SCNu64 " objects, %" SCNu64 " bytes",
                   &objects, &bytes) == 2) {
            sample->dma_buf_objects = objects;
            sample->dma_buf_bytes = bytes;
            break;
        }
    }
    fclose(file);
    return true;
}

static void resource_monitor_sanitize_line(char *line)
{
    size_t index;

    for (index = 0U; line[index] != '\0'; ++index) {
        if (line[index] == '\n' || line[index] == '\r' ||
            line[index] == '\t') {
            line[index] = ' ';
        }
    }
}

static bool resource_monitor_append_ion(ResourceSample *sample,
                                        const char *name,
                                        const char *line)
{
    size_t used;
    int written;

    used = strlen(sample->ion_summary);
    written = snprintf(sample->ion_summary + used,
                       sizeof(sample->ion_summary) - used, "%s%s=%s",
                       used == 0U ? "" : ";", name, line);
    return written >= 0 && (size_t)written < sizeof(sample->ion_summary) - used;
}

static bool resource_monitor_read_ion_file(ResourceSample *sample,
                                           const char *path,
                                           const char *name)
{
    FILE *file;
    char line[160];

    file = fopen(path, "r");
    if (file == NULL || fgets(line, sizeof(line), file) == NULL) {
        if (file != NULL) {
            fclose(file);
        }
        return false;
    }
    fclose(file);
    resource_monitor_sanitize_line(line);
    return resource_monitor_append_ion(sample, name, line);
}

static void resource_monitor_read_ion_dir(ResourceSample *sample,
                                          const char *directory_path,
                                          unsigned int depth)
{
    DIR *directory;
    struct dirent *entry;

    if (depth > 1U) {
        return;
    }
    directory = opendir(directory_path);
    if (directory == NULL) {
        return;
    }
    while ((entry = readdir(directory)) != NULL) {
        char path[256];
        char name[128];
        struct stat status;
        int written;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        written = snprintf(path, sizeof(path), "%s/%s", directory_path,
                           entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(path) ||
            stat(path, &status) != 0) {
            continue;
        }
        written = snprintf(name, sizeof(name), "%s/%s", directory_path,
                           entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(name)) {
            continue;
        }
        if (S_ISREG(status.st_mode) || S_ISCHR(status.st_mode) ||
            S_ISLNK(status.st_mode)) {
            (void)resource_monitor_read_ion_file(sample, path, name);
        } else if (S_ISDIR(status.st_mode)) {
            resource_monitor_read_ion_dir(sample, path, depth + 1U);
        }
    }
    closedir(directory);
}

static void resource_monitor_read_ion(ResourceSample *sample)
{
    static const char *const known_files[] = {
        "/sys/kernel/debug/ion/heaps/system",
        "/sys/kernel/debug/ion/heaps/cma",
        "/sys/kernel/debug/ion/ion_heaps/system",
        "/sys/kernel/debug/ion/ion_heaps/cma"
    };
    size_t index;

    for (index = 0U; index < sizeof(known_files) / sizeof(known_files[0]);
         ++index) {
        (void)resource_monitor_read_ion_file(sample, known_files[index],
                                             known_files[index]);
    }
    resource_monitor_read_ion_dir(sample, "/sys/kernel/debug/ion/heaps", 0U);
    resource_monitor_read_ion_dir(sample, "/sys/kernel/debug/ion/ion_heaps",
                                  0U);
    /* The KickPi 5.4 image exposes named heaps directly below /ion. */
    resource_monitor_read_ion_dir(sample, "/sys/kernel/debug/ion", 0U);
    if (sample->ion_summary[0] == '\0') {
        strcpy(sample->ion_summary, "unavailable");
    }
}

static bool resource_monitor_collect(ResourceSample *sample, uint64_t frame)
{
    bool have_rss;

    memset(sample, 0, sizeof(*sample));
    sample->frame = frame;
    sample->fd_count = resource_monitor_count_fds();
    have_rss = resource_monitor_read_rss(sample);
    (void)resource_monitor_read_meminfo(sample);
    (void)resource_monitor_read_dma_buf(sample);
    resource_monitor_read_ion(sample);
    sample->valid = have_rss && sample->fd_count >= 0;
    return sample->valid;
}

static void resource_monitor_print(const ResourceSample *sample)
{
    printf("resource frame=%" PRIu64 " rss_kb=%ld fd=%d "
           "MemFree=%" PRIu64 " MemAvailable=%" PRIu64 " "
           "CmaTotal=%" PRIu64 " CmaFree=%" PRIu64 " "
           "Slab=%" PRIu64 " SReclaimable=%" PRIu64 " "
           "SUnreclaim=%" PRIu64 " dma_buf_count=%" PRIu64 " "
           "dma_buf_bytes=%" PRIu64 " ion=%s\n",
           sample->frame, sample->rss_kb, sample->fd_count,
           sample->have_mem_free ? sample->mem_free_kb : 0U,
           sample->have_mem_available ? sample->mem_available_kb : 0U,
           sample->have_cma_total ? sample->cma_total_kb : 0U,
           sample->have_cma_free ? sample->cma_free_kb : 0U,
           sample->have_slab ? sample->slab_kb : 0U,
           sample->have_sreclaimable ? sample->sreclaimable_kb : 0U,
           sample->have_sunreclaim ? sample->sunreclaim_kb : 0U,
           sample->have_dma_buf ? sample->dma_buf_objects : 0U,
           sample->have_dma_buf ? sample->dma_buf_bytes : 0U,
           sample->ion_summary);
    fflush(stdout);
}

static void resource_monitor_init(ResourceMonitor *monitor,
                                  unsigned int interval)
{
    memset(monitor, 0, sizeof(*monitor));
    monitor->interval = interval;
}

static void resource_monitor_sample(ResourceMonitor *monitor, uint64_t frame,
                                    bool force)
{
    ResourceSample sample;

    if (!force && (monitor->interval == 0U ||
                   (frame != 0U && frame % monitor->interval != 0U))) {
        return;
    }
    if (!resource_monitor_collect(&sample, frame)) {
        fprintf(stderr, "resource monitor: cannot read /proc/self status\n");
        return;
    }
    resource_monitor_print(&sample);
    if (!monitor->have_first) {
        monitor->first = sample;
        monitor->have_first = true;
    }
    monitor->last = sample;
    monitor->have_last = true;
}

static void resource_monitor_finish(ResourceMonitor *monitor,
                                    uint64_t final_frame)
{
    if (!monitor->have_last || monitor->last.frame != final_frame) {
        resource_monitor_sample(monitor, final_frame, true);
    }
    if (monitor->have_first && monitor->have_last &&
        monitor->last.frame > monitor->first.frame) {
        uint64_t frame_delta = monitor->last.frame - monitor->first.frame;
        int64_t cma_delta = 0;
        int64_t dma_object_delta = 0;
        int64_t dma_byte_delta = 0;
        int64_t fd_delta = (int64_t)monitor->last.fd_count -
                           monitor->first.fd_count;
        int64_t rss_delta = (int64_t)monitor->last.rss_kb -
                            monitor->first.rss_kb;

        if (monitor->first.have_cma_free && monitor->last.have_cma_free) {
            cma_delta = (int64_t)monitor->first.cma_free_kb -
                        (int64_t)monitor->last.cma_free_kb;
        }
        if (monitor->first.have_dma_buf && monitor->last.have_dma_buf) {
            dma_object_delta = (int64_t)monitor->last.dma_buf_objects -
                               (int64_t)monitor->first.dma_buf_objects;
            dma_byte_delta = (int64_t)monitor->last.dma_buf_bytes -
                             (int64_t)monitor->first.dma_buf_bytes;
        }
        printf("resource slope frames=%" PRIu64 " cma_free_loss_kb=%" PRId64
               " cma_free_loss_kb_per_frame=%.3f dma_buf_count_delta=%" PRId64
               " dma_buf_bytes_delta=%" PRId64 " fd_delta=%" PRId64
               " rss_delta_kb=%" PRId64 "\n", frame_delta, cma_delta,
               (double)cma_delta / (double)frame_delta, dma_object_delta,
               dma_byte_delta, fd_delta, rss_delta);
    }
}

#endif
