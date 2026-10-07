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
    if (!center || !cfg) return -1;
    memset(center, 0, sizeof(*center));
    center->cfg = *cfg;
    center->next_parcel_id = 1;
    
    // Выделяем входные накопители под каждую линию
    center->input_buffers = (fifo_queue_t *)calloc(cfg->num_input_lines, sizeof(fifo_queue_t));
    center->next_arrival_tick = (uint32_t *)calloc(cfg->num_input_lines, sizeof(uint32_t));
    for (uint32_t i = 0; i < cfg->num_input_lines; i++) {
        fifo_init(&center->input_buffers[i], cfg->input_buffer_cap);
        center->next_arrival_tick[i] = 1 + (rand() % (cfg->arrival_interval_max - cfg->arrival_interval_min + 1))
                                      + cfg->arrival_interval_min;
    }
    
    // Сканеры
    center->scanners = (scanner_unit_t *)calloc(cfg->num_scanners, sizeof(scanner_unit_t));
    for (uint32_t i = 0; i < cfg->num_scanners; i++) {
        center->scanners[i].id = i + 1;
    }
    
    // Буфер рескана и промежуточный буфер
    fifo_init(&center->rescan_buffer, cfg->intermediate_buffer_cap * 2 + 4);
    fifo_init(&center->intermediate_buffer, cfg->intermediate_buffer_cap);
    
    // Сортировщики
    center->sorters = (sorter_unit_t *)calloc(cfg->num_sorters, sizeof(sorter_unit_t));
    for (uint32_t i = 0; i < cfg->num_sorters; i++) {
        center->sorters[i].id = i + 1;
    }
    
    // Конвейерные ленты для направлений
    center->conveyors = (conveyor_line_t *)calloc(cfg->num_destinations, sizeof(conveyor_line_t));
    for (uint32_t d = 0; d < cfg->num_destinations; d++) {
        center->conveyors[d].destination_id = d;
        center->conveyors[d].length = cfg->conveyor_length;
        center->conveyors[d].cells = (conveyor_cell_t *)calloc(cfg->conveyor_length, sizeof(conveyor_cell_t));
    }
    
    // Выходные накопители
    center->output_bins = (fifo_queue_t *)calloc(cfg->num_destinations, sizeof(fifo_queue_t));
    for (uint32_t d = 0; d < cfg->num_destinations; d++) {
        fifo_init(&center->output_bins[d], cfg->output_bin_cap);
    }
    
    // Зона ручной проверки отбракованных посылок
    fifo_init(&center->manual_inspection_bin, cfg->manual_buffer_cap);
    return 0;
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

// Проверка инвариантов задачи на каждом такте
bool center_verify_invariants(const sort_center_t *center, char *err_msg, size_t err_size) {
    if (!center) return false;
    
    // Вместимость входных буферов не должна превышаться
    for (uint32_t i = 0; i < center->cfg.num_input_lines; i++) {
        if (center->input_buffers[i].count > center->input_buffers[i].capacity) {
            snprintf(err_msg, err_size, "Переполнение входного буфера линии %u (%u > %u)",
                     i + 1, center->input_buffers[i].count, center->input_buffers[i].capacity);
            return false;
        }
    }
    
    // Вместимость промежуточного накопителя
    if (center->intermediate_buffer.count > center->intermediate_buffer.capacity) {
        snprintf(err_msg, err_size, "Переполнение промежуточного накопителя (%u > %u)",
                 center->intermediate_buffer.count, center->intermediate_buffer.capacity);
        return false;
    }
    
    // Сканеры и сортировщики обрабатывают не более 1 посылки за раз
    for (uint32_t i = 0; i < center->cfg.num_scanners; i++) {
        if (center->scanners[i].is_busy && center->scanners[i].current_parcel.id == 0) {
            snprintf(err_msg, err_size, "Сканер %u занят, но ID посылки равен 0", i + 1);
            return false;
        }
    }
    for (uint32_t i = 0; i < center->cfg.num_sorters; i++) {
        if (center->sorters[i].is_busy && center->sorters[i].current_parcel.id == 0) {
            snprintf(err_msg, err_size, "Сортировщик %u занят, но ID посылки равен 0", i + 1);
            return false;
        }
    }
    
    // Посылка может быть на конвейере только если направление уже распознано
    for (uint32_t d = 0; d < center->cfg.num_destinations; d++) {
        for (uint32_t c = 0; c < center->conveyors[d].length; c++) {
            if (center->conveyors[d].cells[c].is_occupied) {
                if (!center->conveyors[d].cells[c].parcel.destination_recognized) {
                    snprintf(err_msg, err_size, "Посылка #%u попала на конвейер без распознанного направления",
                             center->conveyors[d].cells[c].parcel.id);
                    return false;
                }
                if (center->conveyors[d].cells[c].parcel.target_destination != d) {
                    snprintf(err_msg, err_size, "Посылка #%u (цель %u) оказалась на конвейере %u",
                             center->conveyors[d].cells[c].parcel.id,
                             center->conveyors[d].cells[c].parcel.target_destination + 1, d + 1);
                    return false;
                }
            }
        }
    }
    return true;
}