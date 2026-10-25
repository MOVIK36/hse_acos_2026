#ifndef CENTER_H
#define CENTER_H

#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include "config.h"
#include "parcel.h"

typedef struct sort_center sort_center_t;

// Данные посылки, её поток и уведомление об окончании обработки.
typedef struct parcel_job {
    parcel_t data;
    sort_center_t *center;
    pthread_t thread;
    sem_t done;
    struct parcel_job *next_finished;
} parcel_job_t;

// Кольцевая очередь указателей на посылки.
// Индексы и count защищаются общим mutex центра.
typedef struct {
    parcel_job_t **items;
    uint32_t capacity, head, tail, count;
    sem_t free_slots;
} fifo_queue_t;

typedef struct {
    parcel_job_t **cells;
    sem_t entry_free; // Первая ячейка ленты свободна.
    sem_t output_free;
    bool initialized;
    uint32_t output_count;
    uint32_t delivered;
} conveyor_line_t;

struct sort_center {
    sim_config_t cfg;
    pthread_mutex_t mutex;
    bool stop, failed;
    struct timespec started_at;
    fifo_queue_t *inputs;
    fifo_queue_t intermediate;
    conveyor_line_t *conveyors;
    sem_t scan_jobs;       // Суммарное количество посылок во всех входных очередях.
    sem_t sort_jobs;       // Посылки в промежуточной очереди.
    sem_t manual_free;
    sem_t finished;        // Завершился поток посылки, можно выполнить join.
    parcel_job_t *finished_head, *finished_tail;
    uint32_t next_input_line;
    uint32_t total_arrived, total_delivered, total_manual_rejected;
    uint32_t total_shipped, total_manual_serviced, manual_count;
    uint64_t total_rescans, total_waits, sum_transit_ticks;
};

int center_init(sort_center_t *c, const sim_config_t *cfg);
void center_free(sort_center_t *c);

// Эти операции вызываются только под mutex центра.
void fifo_push(fifo_queue_t *q, parcel_job_t *p);
parcel_job_t *fifo_pop(fifo_queue_t *q);

#endif
