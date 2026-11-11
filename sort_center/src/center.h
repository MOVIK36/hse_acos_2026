#ifndef CENTER_H
#define CENTER_H

#include <semaphore.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <time.h>
#include "config.h"
#include "parcel.h"

#define MAX_LINES 100
#define MAX_CAPACITY 10000
#define MAX_PARCELS 128
#define MAX_WORKERS 401
#define MAX_SEMAPHORES 640

typedef struct {
    sem_t value;
    unsigned index;
} ipc_sem_t;

// В общей памяти хранятся номера ячеек, а не адреса другого процесса.
// Ноль означает пустую ячейку; посылки имеют номера 1..MAX_PARCELS.
typedef struct {
    uint32_t items[MAX_CAPACITY];
    uint32_t capacity, head, tail, count;
    ipc_sem_t free_slots;
} fifo_queue_t;

typedef struct {
    parcel_t data;
    ipc_sem_t done;
    unsigned state; // 0 — свободна, 1 — запрос родителю, 2 — процесс создан.
} parcel_job_t;

typedef struct {
    uint32_t cells[MAX_CAPACITY];
    ipc_sem_t entry_free, output_free;
    uint32_t output_count, delivered;
} conveyor_line_t;

typedef struct {
    pid_t pid;
    bool done;
} worker_info_t;

typedef struct sort_center {
    sim_config_t cfg;
    atomic_bool ready;
    bool stop, failed;
    unsigned sem_count, registered;
    pid_t owner;
    ipc_sem_t mutex, log_mutex, parcel_free;
    struct timespec started_at;
    fifo_queue_t inputs[MAX_LINES], intermediate;
    conveyor_line_t conveyors[MAX_LINES];
    parcel_job_t parcels[MAX_PARCELS];
    worker_info_t workers[MAX_WORKERS];
    ipc_sem_t scan_jobs, sort_jobs, manual_free;
    uint32_t next_input_line;
    uint32_t total_arrived, total_delivered, total_manual_rejected;
    uint32_t total_shipped, total_manual_serviced, manual_count;
    uint64_t total_rescans, total_waits, sum_transit_ticks;
} sort_center_t;

// Дескрипторы именованных семафоров хранятся только в локальной памяти.
int center_create(const sim_config_t *cfg, sort_center_t **c);
int center_open(const char *session, sort_center_t **c);
void center_close(sort_center_t *c, bool owner);
sem_t *ipc_sem(ipc_sem_t *s);
void center_lock(sort_center_t *c);
void center_unlock(sort_center_t *c);
void fifo_push(fifo_queue_t *q, sort_center_t *c, parcel_job_t *p);
parcel_job_t *fifo_pop(fifo_queue_t *q, sort_center_t *c);

#endif
