#define _POSIX_C_SOURCE 200809L
#include "center.h"
#include <stdlib.h>
#include <string.h>

static int fifo_init(fifo_queue_t *q, uint32_t capacity) {
    q->items = calloc(capacity, sizeof(*q->items));
    if (!q->items) return -1;
    if (sem_init(&q->free_slots, 0, capacity) != 0) goto fail;
    q->capacity = capacity;
    return 0;
fail:
    free(q->items);
    q->items = NULL;
    return -1;
}

static void fifo_free(fifo_queue_t *q) {
    if (!q->items) return;
    sem_destroy(&q->free_slots);
    free(q->items);
}

void fifo_push(fifo_queue_t *q, parcel_job_t *p) {
    q->items[q->tail] = p;
    q->tail = (q->tail + 1) % q->capacity;
    ++q->count;
}

parcel_job_t *fifo_pop(fifo_queue_t *q) {
    parcel_job_t *p = q->items[q->head];
    q->head = (q->head + 1) % q->capacity;
    --q->count;
    return p;
}

int center_init(sort_center_t *c, const sim_config_t *cfg) {
    memset(c, 0, sizeof(*c));
    c->cfg = *cfg;
    if (pthread_mutex_init(&c->mutex, NULL) != 0) return -1;
    if (sem_init(&c->scan_jobs, 0, 0) != 0) goto fail_mutex;
    if (sem_init(&c->sort_jobs, 0, 0) != 0) goto fail_scan;
    if (sem_init(&c->manual_free, 0, cfg->manual_buffer_cap) != 0) goto fail_sort;
    if (sem_init(&c->finished, 0, 0) != 0) goto fail_manual;
    c->inputs = calloc(cfg->num_input_lines, sizeof(*c->inputs));
    c->conveyors = calloc(cfg->num_destinations, sizeof(*c->conveyors));
    if (!c->inputs || !c->conveyors) goto fail;
    for (uint32_t i = 0; i < cfg->num_input_lines; ++i)
        if (fifo_init(&c->inputs[i], cfg->input_buffer_cap) != 0) goto fail;
    if (fifo_init(&c->intermediate, cfg->intermediate_buffer_cap) != 0) goto fail;
    for (uint32_t d = 0; d < cfg->num_destinations; ++d) {
        conveyor_line_t *line = &c->conveyors[d];
        line->cells = calloc(cfg->conveyor_length, sizeof(*line->cells));
        if (!line->cells) goto fail;
        if (sem_init(&line->entry_free, 0, 1) != 0) goto fail;
        if (sem_init(&line->output_free, 0, cfg->output_bin_cap) != 0) {
            sem_destroy(&line->entry_free);
            goto fail;
        }
        line->initialized = true;
    }
    clock_gettime(CLOCK_MONOTONIC, &c->started_at);
    return 0;
fail:
    center_free(c);
    return -1;
fail_manual:
    sem_destroy(&c->manual_free);
fail_sort:
    sem_destroy(&c->sort_jobs);
fail_scan:
    sem_destroy(&c->scan_jobs);
fail_mutex:
    pthread_mutex_destroy(&c->mutex);
    return -1;
}

void center_free(sort_center_t *c) {
    if (c->inputs) {
        for (uint32_t i = 0; i < c->cfg.num_input_lines; ++i) fifo_free(&c->inputs[i]);
        free(c->inputs);
    }
    fifo_free(&c->intermediate);
    if (c->conveyors) {
        for (uint32_t d = 0; d < c->cfg.num_destinations; ++d) {
            if (c->conveyors[d].initialized) {
                sem_destroy(&c->conveyors[d].entry_free);
                sem_destroy(&c->conveyors[d].output_free);
            }
            free(c->conveyors[d].cells);
        }
        free(c->conveyors);
    }
    sem_destroy(&c->finished);
    sem_destroy(&c->manual_free);
    sem_destroy(&c->sort_jobs);
    sem_destroy(&c->scan_jobs);
    pthread_mutex_destroy(&c->mutex);
}
