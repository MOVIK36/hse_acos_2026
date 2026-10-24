#ifndef CENTER_H
#define CENTER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "parcel.h"
#include "config.h"

// Ограниченная кольцевая очередь FIFO
typedef struct {
    parcel_t *items;
    uint32_t capacity;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint32_t total_pushed;
    uint32_t total_popped;
} fifo_queue_t;

// Сканер штрихкодов
typedef struct {
    uint32_t id;
    bool is_busy;
    bool result_ready; // Результат чтения фиксируется один раз до передачи посылки
    uint32_t next_input_line;
    parcel_t current_parcel;
    uint32_t remaining_ticks;
    uint32_t total_scanned;
    uint32_t total_errors;
} scanner_unit_t;

// Сортировочный пост
typedef struct {
    uint32_t id;
    bool is_busy;
    parcel_t current_parcel;
    uint32_t remaining_ticks;
    uint32_t total_sorted;
} sorter_unit_t;

// Ячейка конвейера
typedef struct {
    parcel_t parcel;
    bool is_occupied;
    uint32_t elapsed_ticks;
} conveyor_cell_t;

// Конвейерная линия для конкретного направления
typedef struct {
    uint32_t destination_id;
    conveyor_cell_t *cells;
    uint32_t length;
    uint32_t parcels_in_flight;
    uint32_t total_transported;
} conveyor_line_t;

// Общая структура сортировочного центра
typedef struct {
    sim_config_t cfg;
    
    // Буферы и устройства
    fifo_queue_t *input_buffers;
    scanner_unit_t *scanners;
    fifo_queue_t intermediate_buffer;
    sorter_unit_t *sorters;
    conveyor_line_t *conveyors;
    fifo_queue_t *output_bins;
    fifo_queue_t manual_inspection_bin;
    fifo_queue_t rescan_buffer;
    
    // Статистика
    uint32_t current_tick;
    uint32_t total_arrived;
    uint32_t total_delivered;
    uint32_t total_manual_rejected;
    uint64_t total_rescans_performed;
    uint64_t total_stalls_experienced;
    uint32_t total_shipped;
    uint32_t total_manual_serviced;
    uint64_t sum_transit_time_ticks;
    
    // Расписание прихода новых посылок
    uint32_t *next_arrival_tick;
    uint32_t next_parcel_id;
} sort_center_t;

int center_init(sort_center_t *center, const sim_config_t *cfg);
void center_free(sort_center_t *center);

int fifo_init(fifo_queue_t *q, uint32_t capacity);
void fifo_free(fifo_queue_t *q);
bool fifo_is_full(const fifo_queue_t *q);
bool fifo_is_empty(const fifo_queue_t *q);
bool fifo_push(fifo_queue_t *q, const parcel_t *p);
bool fifo_pop(fifo_queue_t *q, parcel_t *out_p);
bool fifo_peek(const fifo_queue_t *q, parcel_t *out_p);

bool center_verify_invariants(const sort_center_t *center, char *err_msg, size_t err_size);

#endif /* CENTER_H */
