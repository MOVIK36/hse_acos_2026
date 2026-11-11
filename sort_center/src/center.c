#define _POSIX_C_SOURCE 200809L
#include "center.h"
#include "io_logger.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static sem_t *handles[MAX_SEMAPHORES];
static char prefix[80];
static bool named;

static void sem_name(char *name, size_t size, unsigned index) {
    snprintf(name, size, "%s_s%u", prefix, index);
}

sem_t *ipc_sem(ipc_sem_t *s) {
    return named ? handles[s->index] : &s->value;
}

void center_lock(sort_center_t *c) {
    while (sem_wait(ipc_sem(&c->mutex)) != 0 && errno == EINTR) {}
}

void center_unlock(sort_center_t *c) {
    sem_post(ipc_sem(&c->mutex));
}

static int init_sem(sort_center_t *c, ipc_sem_t *s, unsigned value) {
    s->index = c->sem_count;
    if (c->sem_count >= MAX_SEMAPHORES) return -1;
    if (named) {
        char name[100];
        sem_name(name, sizeof(name), s->index);
        handles[s->index] = sem_open(name, O_CREAT | O_EXCL, 0600, value);
        if (handles[s->index] == SEM_FAILED) {
            handles[s->index] = NULL;
            return -1;
        }
    } else if (sem_init(&s->value, 1, value) != 0) return -1;
    // Адрес пригоден для sem_destroy только в создавшем процессе.
    if (!named) handles[s->index] = &s->value;
    ++c->sem_count;
    return 0;
}

static int init_fifo(sort_center_t *c, fifo_queue_t *q, uint32_t capacity) {
    q->capacity = capacity;
    return init_sem(c, &q->free_slots, capacity);
}

void fifo_push(fifo_queue_t *q, sort_center_t *c, parcel_job_t *p) {
    q->items[q->tail] = (uint32_t)(p - c->parcels) + 1;
    q->tail = (q->tail + 1) % q->capacity;
    ++q->count;
}

parcel_job_t *fifo_pop(fifo_queue_t *q, sort_center_t *c) {
    uint32_t slot = q->items[q->head];
    q->head = (q->head + 1) % q->capacity;
    --q->count;
    return &c->parcels[slot - 1];
}

int center_create(const sim_config_t *cfg, sort_center_t **result) {
    snprintf(prefix, sizeof(prefix), "/sc_%s", cfg->session);
    named = strcmp(cfg->ipc_mode, "unnamed") != 0;
    int fd = shm_open(prefix, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) return -1;
    if (ftruncate(fd, sizeof(sort_center_t)) != 0) {
        close(fd);
        shm_unlink(prefix);
        return -1;
    }
    sort_center_t *c = mmap(NULL, sizeof(*c), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (c == MAP_FAILED) { shm_unlink(prefix); return -1; }
    memset(c, 0, sizeof(*c));
    c->cfg = *cfg;
    c->owner = getpid();
    if (init_sem(c, &c->mutex, 1) || init_sem(c, &c->log_mutex, 1) ||
        init_sem(c, &c->parcel_free, MAX_PARCELS) ||
        init_sem(c, &c->scan_jobs, 0) || init_sem(c, &c->sort_jobs, 0) ||
        init_sem(c, &c->manual_free, cfg->manual_buffer_cap)) goto fail;
    for (uint32_t i = 0; i < cfg->num_input_lines; ++i)
        if (init_fifo(c, &c->inputs[i], cfg->input_buffer_cap)) goto fail;
    if (init_fifo(c, &c->intermediate, cfg->intermediate_buffer_cap)) goto fail;
    for (uint32_t i = 0; i < cfg->num_destinations; ++i) {
        if (init_sem(c, &c->conveyors[i].entry_free, 1) ||
            init_sem(c, &c->conveyors[i].output_free, cfg->output_bin_cap)) goto fail;
    }
    for (unsigned i = 0; i < MAX_PARCELS; ++i)
        if (init_sem(c, &c->parcels[i].done, 0)) goto fail;
    clock_gettime(CLOCK_MONOTONIC, &c->started_at);
    *result = c;
    return 0;
fail: {
    int saved_errno = errno;
    center_close(c, true);
    errno = saved_errno;
    return -1;
}
}

int center_open(const char *session, sort_center_t **result) {
    snprintf(prefix, sizeof(prefix), "/sc_%s", session);
    named = true;
    int fd = shm_open(prefix, O_RDWR, 0600);
    if (fd < 0) return -1;
    // Сначала проверяем размер и признак готовности общей памяти.
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size != (off_t)sizeof(sort_center_t)) {
        close(fd);
        return -1;
    }
    sort_center_t *c = mmap(NULL, sizeof(*c), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (c == MAP_FAILED) return -1;
    if (!atomic_load(&c->ready) || strcmp(c->cfg.ipc_mode, "independent") != 0) {
        munmap(c, sizeof(*c));
        return -1;
    }
    for (unsigned i = 0; i < c->sem_count; ++i) {
        char name[100];
        sem_name(name, sizeof(name), i);
        handles[i] = sem_open(name, 0);
        if (handles[i] == SEM_FAILED) {
            handles[i] = NULL;
            center_close(c, false);
            return -1;
        }
    }
    *result = c;
    return 0;
}

void center_close(sort_center_t *c, bool owner) {
    logger_set_semaphore(NULL);
    for (unsigned i = 0; i < c->sem_count; ++i) {
        if (named) {
            if (handles[i]) sem_close(handles[i]);
            if (owner) {
                char name[100];
                sem_name(name, sizeof(name), i);
                sem_unlink(name);
            }
        } else if (owner) sem_destroy(handles[i]);
        handles[i] = NULL;
    }
    munmap(c, sizeof(*c));
    if (owner) shm_unlink(prefix);
}
