#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "center.h"

int fifo_init(fifo_queue_t *q, uint32_t capacity) {
    if (!q || capacity == 0) return -1;
    q->items = (parcel_t *)calloc(capacity, sizeof(parcel_t));
    if (!q->items) return -1;
    q->capacity = capacity;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->total_pushed = 0;
    q->total_popped = 0;
    return 0;
}

void fifo_free(fifo_queue_t *q) {
    if (q && q->items) {
        free(q->items);
        q->items = NULL;
    }
}

bool fifo_is_full(const fifo_queue_t *q) {
    return q ? (q->count >= q->capacity) : true;
}

bool fifo_is_empty(const fifo_queue_t *q) {
    return q ? (q->count == 0) : true;
}

bool fifo_push(fifo_queue_t *q, const parcel_t *p) {
    if (!q || fifo_is_full(q)) return false;
    q->items[q->tail] = *p;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    q->total_pushed++;
    return true;
}

bool fifo_pop(fifo_queue_t *q, parcel_t *out_p) {
    if (!q || fifo_is_empty(q)) return false;
    if (out_p) {
        *out_p = q->items[q->head];
    }
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    q->total_popped++;
    return true;
}

bool fifo_peek(const fifo_queue_t *q, parcel_t *out_p) {
    if (!q || fifo_is_empty(q)) return false;
    if (out_p) {
        *out_p = q->items[q->head];
    }
    return true;
}

int center_init(sort_center_t *center, const sim_config_t *cfg) {
    if (!center) return -1;
    memset(center, 0, sizeof(*center));
    if (config_validate(cfg) != 0) return -1;
    center->cfg = *cfg;
    center->next_parcel_id = 1;
    center->input_buffers = calloc(cfg->num_input_lines, sizeof(*center->input_buffers));
    center->next_arrival_tick = calloc(cfg->num_input_lines, sizeof(*center->next_arrival_tick));
    center->scanners = calloc(cfg->num_scanners, sizeof(*center->scanners));
    center->sorters = calloc(cfg->num_sorters, sizeof(*center->sorters));
    center->conveyors = calloc(cfg->num_destinations, sizeof(*center->conveyors));
    center->output_bins = calloc(cfg->num_destinations, sizeof(*center->output_bins));
    if (!center->input_buffers || !center->next_arrival_tick || !center->scanners ||
        !center->sorters || !center->conveyors || !center->output_bins) goto fail;

    for (uint32_t i = 0; i < cfg->num_input_lines; ++i) {
        if (fifo_init(&center->input_buffers[i], cfg->input_buffer_cap) != 0) goto fail;
        center->next_arrival_tick[i] = 1 + cfg->arrival_interval_min +
            (uint32_t)rand() % (cfg->arrival_interval_max - cfg->arrival_interval_min + 1);
    }
    for (uint32_t i = 0; i < cfg->num_scanners; ++i) {
        center->scanners[i].id = i + 1;
        center->scanners[i].next_input_line = i % cfg->num_input_lines;
    }
    for (uint32_t i = 0; i < cfg->num_sorters; ++i) center->sorters[i].id = i + 1;
    if (fifo_init(&center->rescan_buffer, cfg->rescan_buffer_cap) != 0 ||
        fifo_init(&center->intermediate_buffer, cfg->intermediate_buffer_cap) != 0 ||
        fifo_init(&center->manual_inspection_bin, cfg->manual_buffer_cap) != 0) goto fail;
    for (uint32_t d = 0; d < cfg->num_destinations; ++d) {
        center->conveyors[d].destination_id = d;
        center->conveyors[d].length = cfg->conveyor_length;
        center->conveyors[d].cells = calloc(cfg->conveyor_length, sizeof(conveyor_cell_t));
        if (!center->conveyors[d].cells || fifo_init(&center->output_bins[d], cfg->output_bin_cap) != 0) goto fail;
    }
    return 0;
fail:
    center_free(center);
    return -1;
}

void center_free(sort_center_t *center) {
    if (!center) return;
    if (center->input_buffers) {
        for (uint32_t i = 0; i < center->cfg.num_input_lines; i++) {
            fifo_free(&center->input_buffers[i]);
        }
        free(center->input_buffers);
        center->input_buffers = NULL;
    }
    if (center->next_arrival_tick) {
        free(center->next_arrival_tick);
        center->next_arrival_tick = NULL;
    }
    if (center->scanners) {
        free(center->scanners);
        center->scanners = NULL;
    }
    fifo_free(&center->rescan_buffer);
    fifo_free(&center->intermediate_buffer);
    if (center->sorters) {
        free(center->sorters);
        center->sorters = NULL;
    }
    if (center->conveyors) {
        for (uint32_t d = 0; d < center->cfg.num_destinations; d++) {
            if (center->conveyors[d].cells) {
                free(center->conveyors[d].cells);
            }
        }
        free(center->conveyors);
        center->conveyors = NULL;
    }
    if (center->output_bins) {
        for (uint32_t d = 0; d < center->cfg.num_destinations; d++) {
            fifo_free(&center->output_bins[d]);
        }
        free(center->output_bins);
        center->output_bins = NULL;
    }
    fifo_free(&center->manual_inspection_bin);
}

// Простые проверки: вместимость, стадия посылки и общий баланс.
static bool valid_parcel(const sort_center_t *c, const parcel_t *p, parcel_stage_t stage) {
    if (!p->id || p->id >= c->next_parcel_id || p->stage != stage ||
        p->source_line >= c->cfg.num_input_lines || p->target_destination >= c->cfg.num_destinations)
        return false;
    if (stage == STAGE_POST_SCAN_BUFFER || stage == STAGE_SORTING ||
        stage == STAGE_CONVEYOR || stage == STAGE_OUTPUT_BIN)
        return p->destination_recognized && p->label_stat == LABEL_OK;
    if (stage == STAGE_MANUAL_INSPECTION)
        return !p->destination_recognized && p->label_stat == LABEL_DAMAGED;
    return true;
}

static bool valid_queue(const sort_center_t *c, const fifo_queue_t *q, parcel_stage_t stage) {
    if (q->count > q->capacity) return false;
    for (uint32_t i = 0; i < q->count; ++i)
        if (!valid_parcel(c, &q->items[(q->head + i) % q->capacity], stage)) return false;
    return true;
}

bool center_verify_invariants(const sort_center_t *c, char *err_msg, size_t err_size) {
    uint64_t stored = c->total_shipped + (uint64_t)c->total_manual_serviced;
    uint64_t output_count = 0;
    for (uint32_t i = 0; i < c->cfg.num_input_lines; ++i) {
        if (!valid_queue(c, &c->input_buffers[i], STAGE_INPUT_BUFFER)) goto invalid;
        stored += c->input_buffers[i].count;
    }
    if (!valid_queue(c, &c->intermediate_buffer, STAGE_POST_SCAN_BUFFER) ||
        !valid_queue(c, &c->rescan_buffer, STAGE_RESCAN_BUFFER) ||
        !valid_queue(c, &c->manual_inspection_bin, STAGE_MANUAL_INSPECTION)) goto invalid;
    stored += c->intermediate_buffer.count + c->rescan_buffer.count + c->manual_inspection_bin.count;
    for (uint32_t i = 0; i < c->cfg.num_scanners; ++i) {
        if (!c->scanners[i].is_busy) continue;
        if (!valid_parcel(c, &c->scanners[i].current_parcel, STAGE_SCANNING)) goto invalid;
        ++stored;
    }
    for (uint32_t i = 0; i < c->cfg.num_sorters; ++i) {
        if (!c->sorters[i].is_busy) continue;
        if (!valid_parcel(c, &c->sorters[i].current_parcel, STAGE_SORTING)) goto invalid;
        ++stored;
    }
    for (uint32_t d = 0; d < c->cfg.num_destinations; ++d) {
        if (!valid_queue(c, &c->output_bins[d], STAGE_OUTPUT_BIN)) goto invalid;
        for (uint32_t i = 0; i < c->output_bins[d].count; ++i) {
            const parcel_t *p = &c->output_bins[d].items[(c->output_bins[d].head + i) % c->output_bins[d].capacity];
            if (p->target_destination != d) goto invalid;
        }
        stored += c->output_bins[d].count;
        output_count += c->output_bins[d].count;
        for (uint32_t i = 0; i < c->conveyors[d].length; ++i) {
            const conveyor_cell_t *cell = &c->conveyors[d].cells[i];
            if (!cell->is_occupied) continue;
            if (!valid_parcel(c, &cell->parcel, STAGE_CONVEYOR) || cell->parcel.target_destination != d) goto invalid;
            ++stored;
        }
    }
    if (stored != c->total_arrived || output_count + c->total_shipped != c->total_delivered ||
        (uint64_t)c->manual_inspection_bin.count + c->total_manual_serviced != c->total_manual_rejected) {
        snprintf(err_msg, err_size, "Нарушен баланс посылок");
        return false;
    }
    return true;
invalid:
    snprintf(err_msg, err_size, "Переполнен накопитель или неверна стадия/направление посылки");
    return false;
}
