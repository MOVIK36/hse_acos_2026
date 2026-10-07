#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "event_sim.h"
#include "io_logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stddef.h>

int sim_init(event_simulation_t *sim, const sim_config_t *cfg) {
    if (!sim || !cfg) return -1;
    sim->status = SIM_STATUS_RUNNING;
    sim->stop_requested = false;
    return center_init(&sim->center, cfg);
}

void sim_cleanup(event_simulation_t *sim) {
    if (sim) {
        center_free(&sim->center);
    }
}

void sim_request_stop(event_simulation_t *sim) {
    if (sim) {
        sim->stop_requested = true;
    }
}

// Прибытие новых посылок на входные линии
static void sim_phase_arrivals(sort_center_t *c) {
    for (uint32_t line = 0; line < c->cfg.num_input_lines; line++) {
        if (!c->cfg.unlimited_mode && c->total_arrived >= c->cfg.num_parcels) {
            continue;
        }
        
        if (c->current_tick >= c->next_arrival_tick[line]) {
            if (!fifo_is_full(&c->input_buffers[line])) {
                parcel_t p;
                memset(&p, 0, sizeof(p));
                p.id = c->next_parcel_id++;
                p.source_line = line;
                p.target_destination = rand() % c->cfg.num_destinations;
                p.label_stat = LABEL_OK;
                p.stage = STAGE_INPUT_BUFFER;
                p.rescan_attempts = 0;
                p.arrival_tick = c->current_tick;
                p.destination_recognized = false;
                
                fifo_push(&c->input_buffers[line], &p);
                c->total_arrived++;
                
                log_event(c->current_tick, "PARCEL_ARRIVAL",
                          "Посылка #%03u прибыла на Линию %u (цель: Направление %u). Буфер линии: %u/%u",
                          p.id, line + 1, p.target_destination + 1,
                          c->input_buffers[line].count, c->input_buffers[line].capacity);
            } else {
                c->total_stalls_experienced++;
                log_event(c->current_tick, "INPUT_STALL",
                          "Затор: Входной буфер линии %u полон (%u/%u), прибытие отложено",
                          line + 1, c->input_buffers[line].count, c->input_buffers[line].capacity);
            }
            
            uint32_t delta = c->cfg.arrival_interval_min;
            if (c->cfg.arrival_interval_max > c->cfg.arrival_interval_min) {
                delta += rand() % (c->cfg.arrival_interval_max - c->cfg.arrival_interval_min + 1);
            }
            c->next_arrival_tick[line] = c->current_tick + delta;
        }
    }
}

// Движение по выходным конвейерам
static void sim_phase_conveyors(sort_center_t *c) {
    for (uint32_t d = 0; d < c->cfg.num_destinations; d++) {
        conveyor_line_t *conv = &c->conveyors[d];
        uint32_t last_idx = conv->length - 1;
        
        if (conv->cells[last_idx].is_occupied) {
            parcel_t *p = &conv->cells[last_idx].parcel;
            if (!fifo_is_full(&c->output_bins[d])) {
                p->stage = STAGE_OUTPUT_BIN;
                p->completion_tick = c->current_tick;
                fifo_push(&c->output_bins[d], p);
                
                c->total_delivered++;
                uint32_t transit_time = p->completion_tick - p->arrival_tick;
                c->sum_transit_time_ticks += transit_time;
                conv->cells[last_idx].is_occupied = false;
                conv->total_transported++;
                
                log_event(c->current_tick, "OUTPUT_DELIVERED",
                          "Посылка #%03u успешно доставлена в Накопитель %u! (В системе: %u тактов, буфер: %u/%u)",
                          p->id, d + 1, transit_time, c->output_bins[d].count, c->output_bins[d].capacity);
            } else {
                p->wait_ticks++;
                c->total_stalls_experienced++;
                log_event(c->current_tick, "OUTPUT_STALL",
                          "Затор: Выходной накопитель %u полон, посылка #%03u застряла на конце ленты",
                          d + 1, p->id);
            }
        }
        
        for (int i = (int)conv->length - 2; i >= 0; i--) {
            if (conv->cells[i].is_occupied && !conv->cells[i + 1].is_occupied) {
                conv->cells[i + 1] = conv->cells[i];
                conv->cells[i + 1].elapsed_ticks++;
                conv->cells[i].is_occupied = false;
            }
        }
    }
}

// Работа сортировочного поста
static void sim_phase_sorters(sort_center_t *c) {
    for (uint32_t s = 0; s < c->cfg.num_sorters; s++) {
        sorter_unit_t *sorter = &c->sorters[s];
        
        if (sorter->is_busy) {
            if (sorter->remaining_ticks > 1) {
                sorter->remaining_ticks--;
            } else {
                uint32_t dest = sorter->current_parcel.target_destination;
                conveyor_line_t *conv = &c->conveyors[dest];
                
                if (!conv->cells[0].is_occupied) {
                    conv->cells[0].parcel = sorter->current_parcel;
                    conv->cells[0].parcel.stage = STAGE_CONVEYOR;
                    conv->cells[0].is_occupied = true;
                    conv->cells[0].elapsed_ticks = 0;
                    
                    sorter->total_sorted++;
                    sorter->is_busy = false;
                    
                    log_event(c->current_tick, "SORT_DISPATCH",
                              "Сортировщик %u: посылка #%03u отправлена на Конвейер %u",
                              sorter->id, sorter->current_parcel.id, dest + 1);
                } else {
                    sorter->current_parcel.wait_ticks++;
                    c->total_stalls_experienced++;
                    log_event(c->current_tick, "CONVEYOR_STALL",
                              "Затор: Вход конвейера %u занят, Сортировщик %u ждет с посылкой #%03u",
                              dest + 1, sorter->id, sorter->current_parcel.id);
                }
            }
        }
        
        if (!sorter->is_busy && !fifo_is_empty(&c->intermediate_buffer)) {
            fifo_pop(&c->intermediate_buffer, &sorter->current_parcel);
            sorter->current_parcel.stage = STAGE_SORTING;
            sorter->is_busy = true;
            sorter->remaining_ticks = c->cfg.sort_duration;
            
            log_event(c->current_tick, "SORT_START",
                      "Сортировщик %u начал обработку посылки #%03u (цель: Направление %u)",
                      sorter->id, sorter->current_parcel.id,
                      sorter->current_parcel.target_destination + 1);
        }
    }
}

// Сканирование штрихкодов и обработка ошибок
static void sim_phase_scanners(sort_center_t *c) {
    for (uint32_t sc = 0; sc < c->cfg.num_scanners; sc++) {
        scanner_unit_t *scanner = &c->scanners[sc];
        
        if (scanner->is_busy) {
            if (scanner->remaining_ticks > 1) {
                scanner->remaining_ticks--;
            } else {
                uint32_t roll = rand() % 100;
                bool is_error = (roll < c->cfg.label_error_prob_pct);
                
                if (!is_error) {
                    scanner->current_parcel.destination_recognized = true;
                    scanner->current_parcel.label_stat = LABEL_OK;
                    
                    if (!fifo_is_full(&c->intermediate_buffer)) {
                        scanner->current_parcel.stage = STAGE_POST_SCAN_BUFFER;
                        fifo_push(&c->intermediate_buffer, &scanner->current_parcel);
                        scanner->total_scanned++;
                        scanner->is_busy = false;
                        
                        log_event(c->current_tick, "SCAN_SUCCESS",
                                  "Сканер %u: штрихкод #%03u успешно прочитан (цель %u) -> в промежуточный буфер (%u/%u)",
                                  scanner->id, scanner->current_parcel.id,
                                  scanner->current_parcel.target_destination + 1,
                                  c->intermediate_buffer.count, c->intermediate_buffer.capacity);
                    } else {
                        scanner->current_parcel.wait_ticks++;
                        c->total_stalls_experienced++;
                        log_event(c->current_tick, "SCAN_BUFFER_STALL",
                                  "Затор: Промежуточный буфер полон (%u/%u), Сканер %u ждет с посылкой #%03u",
                                  c->intermediate_buffer.count, c->intermediate_buffer.capacity,
                                  scanner->id, scanner->current_parcel.id);
                    }
                } else {
                    scanner->total_errors++;
                    scanner->current_parcel.rescan_attempts++;
                    c->total_rescans_performed++;
                    
                    if (scanner->current_parcel.rescan_attempts > c->cfg.max_rescan_attempts) {
                        scanner->current_parcel.label_stat = LABEL_DAMAGED;
                        scanner->current_parcel.stage = STAGE_MANUAL_INSPECTION;
                        scanner->current_parcel.completion_tick = c->current_tick;
                        
                        fifo_push(&c->manual_inspection_bin, &scanner->current_parcel);
                        c->total_manual_rejected++;
                        scanner->is_busy = false;
                        
                        log_event(c->current_tick, "MANUAL_REJECT",
                                  "Брак: Штрихкод #%03u не прочитан за %u попыток -> отправлена в РУЧНУЮ ОБРАБОТКУ (%u/%u)",
                                  scanner->current_parcel.id, c->cfg.max_rescan_attempts,
                                  c->manual_inspection_bin.count, c->manual_inspection_bin.capacity);
                    } else {
                        scanner->current_parcel.label_stat = LABEL_UNREADABLE;
                        fifo_push(&c->rescan_buffer, &scanner->current_parcel);
                        scanner->is_busy = false;
                        
                        log_event(c->current_tick, "SCAN_RETRY",
                                  "Сбой чтения: Посылка #%03u направлена на повторный скан (попытка %u/%u)",
                                  scanner->current_parcel.id,
                                  scanner->current_parcel.rescan_attempts,
                                  c->cfg.max_rescan_attempts);
                    }
                }
            }
        }
        
        if (!scanner->is_busy) {
            if (!fifo_is_empty(&c->rescan_buffer)) {
                fifo_pop(&c->rescan_buffer, &scanner->current_parcel);
                scanner->current_parcel.stage = STAGE_SCANNING;
                scanner->is_busy = true;
                scanner->remaining_ticks = c->cfg.scan_duration;
                
                log_event(c->current_tick, "SCAN_START_RESCAN",
                          "Сканер %u: взята на ПОВТОРНЫЙ скан посылка #%03u",
                          scanner->id, scanner->current_parcel.id);
            } else {
                for (uint32_t l = 0; l < c->cfg.num_input_lines; l++) {
                    uint32_t line_idx = (sc + l) % c->cfg.num_input_lines;
                    if (!fifo_is_empty(&c->input_buffers[line_idx])) {
                        fifo_pop(&c->input_buffers[line_idx], &scanner->current_parcel);
                        scanner->current_parcel.stage = STAGE_SCANNING;
                        scanner->is_busy = true;
                        scanner->remaining_ticks = c->cfg.scan_duration;
                        
                        log_event(c->current_tick, "SCAN_START",
                                  "Сканер %u: взята посылка #%03u из Линии %u",
                                  scanner->id, scanner->current_parcel.id, line_idx + 1);
                        break;
                    }
                }
            }
        }
    }
}

bool sim_step(event_simulation_t *sim) {
    if (!sim || sim->status != SIM_STATUS_RUNNING) return false;
    sort_center_t *c = &sim->center;
    c->current_tick++;
    
    sim_phase_conveyors(c);
    sim_phase_sorters(c);
    sim_phase_scanners(c);
    sim_phase_arrivals(c);
    
    char err_msg[256];
    if (!center_verify_invariants(c, err_msg, sizeof(err_msg))) {
        log_event(c->current_tick, "INVARIANT_FAIL", "%s", err_msg);
        sim->status = SIM_STATUS_INVARIANT_ERROR;
        return false;
    }
    
    if (!c->cfg.unlimited_mode) {
        if (c->total_delivered + c->total_manual_rejected >= c->cfg.num_parcels) {
            sim->status = SIM_STATUS_COMPLETED;
            return false;
        }
    }
    
    if (sim->stop_requested) {
        sim->status = SIM_STATUS_INTERRUPTED;
        return false;
    }
    
    if (c->cfg.step_delay_ms > 0) {
        struct timespec ts;
        ts.tv_sec = c->cfg.step_delay_ms / 1000;
        ts.tv_nsec = (c->cfg.step_delay_ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }
    
    return true;
}

sim_status_t sim_run(event_simulation_t *sim) {
    if (!sim) return SIM_STATUS_INVARIANT_ERROR;
    log_event(0, "SIM_START", "Старт симуляции сортировочного центра...");
    while (sim_step(sim)) {}
    
    switch (sim->status) {
        case SIM_STATUS_COMPLETED:
            log_event(sim->center.current_tick, "SIM_END",
                      "Моделирование завершено: все %u посылок обработаны.",
                      sim->center.cfg.num_parcels);
            break;
        case SIM_STATUS_INTERRUPTED:
            log_event(sim->center.current_tick, "SIM_INTERRUPT",
                      "Остановлено пользователем (сигнал SIGINT / Ctrl+C).");
            break;
        case SIM_STATUS_INVARIANT_ERROR:
            log_event(sim->center.current_tick, "SIM_ABORT",
                      "Ошибка: нарушен инвариант!");
            break;
        default:
            break;
    }
    return sim->status;
}

void sim_print_summary(const event_simulation_t *sim, int out_fd) {
    if (!sim) return;
    const sort_center_t *c = &sim->center;
    
    dprintf(out_fd, "\n==================== ИТОГОВАЯ СТАТИСТИКА ====================\n");
    dprintf(out_fd, "  Статус:                      %s\n",
            sim->status == SIM_STATUS_COMPLETED ? "ЗАВЕРШЕНО УСПЕШНО" :
            sim->status == SIM_STATUS_INTERRUPTED ? "ПРЕРВАНО ПО SIGINT" : "ОШИБКА");
    dprintf(out_fd, "  Тактов работы:               %u\n", c->current_tick);
    dprintf(out_fd, "  Прибыло посылок:             %u\n", c->total_arrived);
    dprintf(out_fd, "  Доставлено в накопители:     %u (%.1f%%)\n",
            c->total_delivered,
            c->total_arrived > 0 ? (100.0 * c->total_delivered / c->total_arrived) : 0.0);
    dprintf(out_fd, "  Отбраковано (ручная зона):   %u (%.1f%%)\n",
            c->total_manual_rejected,
            c->total_arrived > 0 ? (100.0 * c->total_manual_rejected / c->total_arrived) : 0.0);
    dprintf(out_fd, "  Повторных сканирований:      %u\n", c->total_rescans_performed);
    dprintf(out_fd, "  Тактов заторов (простоя):    %u\n", c->total_stalls_experienced);
    
    if (c->total_delivered > 0) {
        double avg_time = (double)c->sum_transit_time_ticks / c->total_delivered;
        dprintf(out_fd, "  Среднее время доставки:      %.2f тактов\n", avg_time);
    }
    
    dprintf(out_fd, "\n--- Накопители по направлениям ---\n");
    for (uint32_t d = 0; d < c->cfg.num_destinations; d++) {
        dprintf(out_fd, "  Направление #%u: доставлено %u (в буфере: %u/%u)\n",
                d + 1, c->conveyors[d].total_transported,
                c->output_bins[d].count, c->output_bins[d].capacity);
    }
    dprintf(out_fd, "  Ручная зона: %u посылок\n", c->manual_inspection_bin.count);
    dprintf(out_fd, "=============================================================\n\n");
}