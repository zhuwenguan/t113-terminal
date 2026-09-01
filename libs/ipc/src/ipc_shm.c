#define _POSIX_C_SOURCE 200809L
#include <race/shared_state.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *object_name(const char *name) { return name ? name : RACE_SHARED_STATE_NAME; }
static void reset_handle(RaceSharedStateHandle *h) { memset(h, 0, sizeof(*h)); h->fd = -1; }

/* Keep the writer contract enforceable when daemons are launched manually.
 * Readers intentionally do not take this advisory lock. */
static int lock_writer_fd(int fd) {
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &lock) < 0) {
        if (errno == EACCES || errno == EAGAIN) errno = EBUSY;
        return -1;
    }
    return 0;
}

static void initialize_mapping(RaceSharedState *mapping) {
    uint32_t seq = atomic_load_explicit(&mapping->seq, memory_order_relaxed);
    if (seq & 1U) seq++;
    atomic_store_explicit(&mapping->seq, seq + 1U, memory_order_release);
    atomic_thread_fence(memory_order_seq_cst);
    mapping->magic = RACE_SHM_MAGIC;
    mapping->version = RACE_SHM_VERSION;
    mapping->header_size = (uint16_t)offsetof(RaceSharedState, state);
    mapping->total_size = sizeof(RaceSharedState);
    mapping->state_size = sizeof(VehicleState);
    mapping->flags = 0U;
    mapping->publish_mono_us = 0U;
    memset(&mapping->state, 0, sizeof(mapping->state));
    atomic_store_explicit(&mapping->seq, seq + 2U, memory_order_release);
}

int race_shared_state_validate(const RaceSharedState *s) {
    if (!s || s->magic != RACE_SHM_MAGIC || s->version != RACE_SHM_VERSION ||
        s->header_size != offsetof(RaceSharedState, state) ||
        s->total_size != sizeof(RaceSharedState) || s->state_size != sizeof(VehicleState)) {
        errno = EPROTO; return -1;
    }
    return 0;
}

int race_shared_state_writer_open(RaceSharedStateHandle *h, const char *name) {
    _Atomic uint32_t local_seq;
    if (!h) { errno = EINVAL; return -1; }
    reset_handle(h);
    if (!atomic_is_lock_free(&local_seq)) { errno = ENOTSUP; return -1; }
    h->fd = shm_open(object_name(name), O_RDWR | O_CREAT, 0644);
    if (h->fd < 0 || lock_writer_fd(h->fd) < 0 ||
        ftruncate(h->fd, sizeof(RaceSharedState)) < 0) goto fail;
    h->mapping = mmap(NULL, sizeof(RaceSharedState), PROT_READ | PROT_WRITE,
                      MAP_SHARED, h->fd, 0);
    if (h->mapping == MAP_FAILED) { h->mapping = NULL; goto fail; }
    if (!atomic_is_lock_free(&h->mapping->seq)) { errno = ENOTSUP; goto fail; }
    initialize_mapping(h->mapping);
    h->writer = true; return 0;
fail: race_shared_state_close(h); return -1;
}

int race_shared_state_reader_open(RaceSharedStateHandle *h, const char *name) {
    struct stat st;
    if (!h) { errno = EINVAL; return -1; }
    reset_handle(h); memset(&st, 0, sizeof(st));
    h->fd = shm_open(object_name(name), O_RDONLY, 0);
    if (h->fd < 0 || fstat(h->fd, &st) < 0) goto fail;
    if (st.st_size != (off_t)sizeof(RaceSharedState)) { errno = EPROTO; goto fail; }
    h->mapping = mmap(NULL, sizeof(RaceSharedState), PROT_READ, MAP_SHARED, h->fd, 0);
    if (h->mapping == MAP_FAILED) { h->mapping = NULL; goto fail; }
    if (race_shared_state_validate(h->mapping) < 0) goto fail;
    return 0;
fail: race_shared_state_close(h); return -1;
}

int race_shared_state_publish(RaceSharedStateHandle *h, const VehicleState *state,
                              uint64_t published) {
    uint32_t seq;
    if (!h || !h->writer || !h->mapping || !state) { errno = EINVAL; return -1; }
    seq = atomic_load_explicit(&h->mapping->seq, memory_order_relaxed);
    if (seq & 1U) seq++;
    atomic_store_explicit(&h->mapping->seq, seq + 1U, memory_order_relaxed);
    /* Full fence is intentional: the odd marker must become observable before
     * any non-atomic payload store can become observable on weakly ordered ARM. */
    atomic_thread_fence(memory_order_seq_cst);
    memcpy(&h->mapping->state, state, sizeof(*state));
    h->mapping->publish_mono_us = published;
    atomic_store_explicit(&h->mapping->seq, seq + 2U, memory_order_release);
    return 0;
}

int race_shared_state_read(const RaceSharedStateHandle *h, VehicleState *state,
                           uint64_t *published, uint32_t *sequence) {
    unsigned retry;
    if (!h || !h->mapping || !state) { errno = EINVAL; return -1; }
    for (retry = 0; retry < RACE_SHARED_READ_RETRIES; retry++) {
        uint32_t before = atomic_load_explicit(&h->mapping->seq, memory_order_acquire);
        uint64_t timestamp;
        if (before & 1U) continue;
        memcpy(state, &h->mapping->state, sizeof(*state));
        timestamp = h->mapping->publish_mono_us;
        atomic_thread_fence(memory_order_seq_cst);
        if (before == atomic_load_explicit(&h->mapping->seq, memory_order_acquire)) {
            if (published) *published = timestamp;
            if (sequence) *sequence = before;
            return 0;
        }
    }
    errno = EAGAIN; return -1;
}

bool race_shared_state_is_stale(uint64_t now, uint64_t published, uint64_t threshold) {
    return published == 0U || now < published || now - published > threshold;
}

void race_shared_state_close(RaceSharedStateHandle *h) {
    if (!h) return;
    if (h->mapping) (void)munmap(h->mapping, sizeof(RaceSharedState));
    if (h->fd >= 0) (void)close(h->fd);
    reset_handle(h);
}
