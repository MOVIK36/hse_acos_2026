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
#include <inttypes.h>

int sim_init(event_simulation_t *sim, const sim_config_t *cfg) {
    if (!sim || !cfg) return -1;
    sim->status = SIM_STATUS_RUNNING;
    sim->stop_requested = false;
    sim->signal_flag = NULL;
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
static bool sim_phase_arrivals(sort_center_t *c) {
    for (uint32_t line = 0; line < c->cfg.num_input_lines; line++) {
        if (!c->cfg.unlimited_mode && c->total_arrived >= c->cfg.num_parcels) {
            continue;
        }
        
        if (c->current_tick >= c->next_arrival_tick[line]) {
            if (!fifo_is_full(&c->input_buffers[line])) {
                parcel_t p;
                memset(&p, 0, sizeof(p));
                p.id = c->next_parcel_id;
                p.source_line = line;
                p.target_destination = rand() % c->cfg.num_destinations;
                p.label_stat = LABEL_OK;
                p.stage = STAGE_INPUT_BUFFER;
                p.rescan_attempts = 0;
                p.arrival_tick = c->current_tick;
                p.destination_recognized = false;
                
                if (c->next_parcel_id == UINT32_MAX) return false;
                if (!fifo_push(&c->input_buffers[line], &p)) return false;
                c->next_parcel_id++;
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
                continue; // Повторить отложенное поступление на следующем такте.
            }
            
            uint32_t delta = c->cfg.arrival_interval_min;
            if (c->cfg.arrival_interval_max > c->cfg.arrival_interval_min) {
                delta += rand() % (c->cfg.arrival_interval_max - c->cfg.arrival_interval_min + 1);
            }
            c->next_arrival_tick[line] = c->current_tick + delta;
        }
    }
    return true;
}

// Дополнение модели: периодическая отгрузка и освобождение ручной зоны.
// Это конечные стоки, а не бесконечные накопители: каждый вынос виден в журнале.
static void sim_phase_service(sort_center_t *c) {
    if (c->current_tick % c->cfg.output_dispatch_interval == 0) {
        for (uint32_t d = 0; d < c->cfg.num_destinations; ++d) {
            parcel_t p;
            while (fifo_pop(&c->output_bins[d], &p)) {
                ++c->total_shipped;
                log_event(c->current_tick, "OUTPUT_SHIPPED", "Посылка #%03u отгружена из накопителя %u", p.id, d + 1);
            }
        }
    }
    if (c->current_tick % c->cfg.manual_service_interval == 0) {
        parcel_t p;
        while (fifo_pop(&c->manual_inspection_bin, &p)) {
            ++c->total_manual_serviced;
            log_event(c->current_tick, "MANUAL_SERVICED", "Посылка #%03u передана из ручной зоны оператору", p.id);
        }
    }
}

// Движение по выходным конвейерам
static void sim_phase_conveyors(sort_center_t *c) {
    for (uint32_t d = 0; d < c->cfg.num_destinations; ++d) {
        conveyor_line_t *conv = &c->conveyors[d];
        // Каждый микрошаг двигает посылку ровно на одну ячейку, включая выход.
        bool output_stalled = false;
        for (uint32_t step = 0; step < c->cfg.conveyor_speed; ++step) {
            conveyor_cell_t *last = &conv->cells[conv->length - 1];
            if (last->is_occupied) {
                if (!fifo_is_full(&c->output_bins[d])) {
                    parcel_t p = last->parcel;
                    p.stage = STAGE_OUTPUT_BIN;
                    p.completion_tick = c->current_tick;
                    if (fifo_push(&c->output_bins[d], &p)) {
                        ++c->total_delivered;
                        c->sum_transit_time_ticks += p.completion_tick - p.arrival_tick;
                        last->is_occupied = false;
                        ++conv->total_transported;
                        --conv->parcels_in_flight;
                        log_event(c->current_tick, "OUTPUT_DELIVERED",
                                  "Посылка #%03u доставлена в накопитель %u (в системе %u тактов, буфер %u/%u)",
                                  p.id, d + 1, p.completion_tick - p.arrival_tick,
                                  c->output_bins[d].count, c->output_bins[d].capacity);
                    }
                } else if (!output_stalled) {
                    output_stalled = true;
                    ++last->parcel.wait_ticks;
                    ++c->total_stalls_experienced;
                    log_event(c->current_tick, "OUTPUT_STALL",
                              "Выходной накопитель %u полон, посылка #%03u ожидает на ленте", d + 1, last->parcel.id);
                }
            }
            for (uint32_t i = conv->length - 1; i > 0; --i) {
                if (conv->cells[i - 1].is_occupied && !conv->cells[i].is_occupied) {
                    conv->cells[i] = conv->cells[i - 1];
                    ++conv->cells[i].elapsed_ticks;
                    conv->cells[i - 1].is_occupied = false;
                }
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
                    
                    conv->parcels_in_flight++;
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

// Завершённое чтение не повторяется, пока посылка ждёт свободного накопителя.
static void sim_phase_scanners(sort_center_t *c) {
    for (uint32_t sc = 0; sc < c->cfg.num_scanners; ++sc) {
        scanner_unit_t *scanner = &c->scanners[sc];
        parcel_t *p = &scanner->current_parcel;
        if (scanner->is_busy && !scanner->result_ready) {
            if (scanner->remaining_ticks > 1) {
                --scanner->remaining_ticks;
            } else {
                scanner->remaining_ticks = 0;
                scanner->result_ready = true;
                log_event(c->current_tick, "SCAN_FINISH", "Сканер %u закончил чтение посылки #%03u (попытка %u)",
                          scanner->id, p->id, p->rescan_attempts + 1);
                if ((uint32_t)rand() % 100 < c->cfg.label_error_prob_pct) {
                    ++scanner->total_errors;
                    p->destination_recognized = false;
                    p->label_stat = p->rescan_attempts >= c->cfg.max_rescan_attempts ? LABEL_DAMAGED : LABEL_UNREADABLE;
                    log_event(c->current_tick, "SCAN_ERROR", "Штрихкод посылки #%03u не прочитан, %s",
                              p->id, p->label_stat == LABEL_DAMAGED ? "повторы исчерпаны" : "требуется повтор");
                } else {
                    p->destination_recognized = true;
                    p->label_stat = LABEL_OK;
                    ++scanner->total_scanned;
                    log_event(c->current_tick, "SCAN_SUCCESS", "Штрихкод посылки #%03u распознан: направление %u", p->id, p->target_destination + 1);
                }
            }
        }
        if (scanner->is_busy && scanner->result_ready) {
            fifo_queue_t *target;
            parcel_stage_t stage;
            const char *tag, *stall;
            if (p->label_stat == LABEL_OK) {
                target = &c->intermediate_buffer; stage = STAGE_POST_SCAN_BUFFER;
                tag = "SCAN_TRANSFER"; stall = "SCAN_BUFFER_STALL";
            } else if (p->label_stat == LABEL_DAMAGED) {
                target = &c->manual_inspection_bin; stage = STAGE_MANUAL_INSPECTION;
                tag = "MANUAL_REJECT"; stall = "MANUAL_STALL";
            } else {
                target = &c->rescan_buffer; stage = STAGE_RESCAN_BUFFER;
                tag = "SCAN_RETRY"; stall = "RESCAN_STALL";
            }
            parcel_t outgoing = *p;
            outgoing.stage = stage;
            if (stage == STAGE_MANUAL_INSPECTION) outgoing.completion_tick = c->current_tick;
            if (fifo_push(target, &outgoing)) {
                if (stage == STAGE_MANUAL_INSPECTION) ++c->total_manual_rejected;
                scanner->is_busy = false;
                log_event(c->current_tick, tag, "Посылка #%03u: сканер %u -> %s (%u/%u)",
                          p->id, scanner->id, parcel_stage_to_string(stage), target->count, target->capacity);
            } else {
                ++p->wait_ticks;
                ++c->total_stalls_experienced;
                log_event(c->current_tick, stall, "Накопитель %s полон, сканер %u удерживает посылку #%03u",
                          parcel_stage_to_string(stage), scanner->id, p->id);
            }
        }
        if (!scanner->is_busy) {
            bool rescan = fifo_pop(&c->rescan_buffer, p);
            bool found = rescan;
            if (!found) {
                for (uint32_t l = 0; l < c->cfg.num_input_lines; ++l) {
                    uint32_t line = (scanner->next_input_line + l) % c->cfg.num_input_lines;
                    if (fifo_pop(&c->input_buffers[line], p)) {
                        scanner->next_input_line = (line + 1) % c->cfg.num_input_lines;
                        found = true;
                        break;
                    }
                }
            }
            if (found) {
                if (rescan) { ++p->rescan_attempts; ++c->total_rescans_performed; }
                p->stage = STAGE_SCANNING;
                scanner->is_busy = true;
                scanner->result_ready = false;
                scanner->remaining_ticks = c->cfg.scan_duration;
                log_event(c->current_tick, rescan ? "SCAN_START_RESCAN" : "SCAN_START",
                          "Сканер %u начал чтение посылки #%03u из линии %u (повторов %u/%u)",
                          scanner->id, p->id, p->source_line + 1, p->rescan_attempts, c->cfg.max_rescan_attempts);
            }
        }
    }
}

bool sim_step(event_simulation_t *sim) {
    if (!sim || sim->status != SIM_STATUS_RUNNING) return false;
    sort_center_t *c = &sim->center;
    if (sim->stop_requested || (sim->signal_flag && *sim->signal_flag)) {
        sim->status = SIM_STATUS_INTERRUPTED;
        return false;
    }
    if (!c->cfg.unlimited_mode && c->total_delivered + c->total_manual_rejected == c->cfg.num_parcels) {
        sim->status = SIM_STATUS_COMPLETED;
        return false;
    }
    // Не допускаем переполнения модельного времени и расписания поступлений.
    if (c->current_tick >= UINT32_MAX - 1000001U) {
        sim->status = SIM_STATUS_RESOURCE_ERROR;
        return false;
    }
    c->current_tick++;

    sim_phase_service(c);
    sim_phase_conveyors(c);
    sim_phase_sorters(c);
    sim_phase_scanners(c);
    if (!sim_phase_arrivals(c)) {
        sim->status = SIM_STATUS_RESOURCE_ERROR;
        return false;
    }
    
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
    
    if (sim->stop_requested || (sim->signal_flag && *sim->signal_flag)) {
        sim->status = SIM_STATUS_INTERRUPTED;
        return false;
    }
    
    // Короткие порции ограничивают ожидание, даже если сигнал пришёл
    // между проверкой флага и входом в nanosleep().
    uint32_t delay_ms = c->cfg.step_delay_ms;
    while (delay_ms && !sim->stop_requested && !(sim->signal_flag && *sim->signal_flag)) {
        uint32_t slice_ms = delay_ms > 50 ? 50 : delay_ms;
        struct timespec ts = {.tv_sec = 0, .tv_nsec = (long)slice_ms * 1000000L};
        nanosleep(&ts, NULL);
        delay_ms -= slice_ms;
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
                      "Остановлено по запросу пользователя (SIGINT/SIGTERM).");
            break;
        case SIM_STATUS_INVARIANT_ERROR:
            log_event(sim->center.current_tick, "SIM_ABORT",
                      "Ошибка: нарушен инвариант!");
            break;
        case SIM_STATUS_RESOURCE_ERROR:
            log_event(sim->center.current_tick, "SIM_ABORT", "Недостаточно ресурсов или исчерпан диапазон счётчиков.");
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
            sim->status == SIM_STATUS_INTERRUPTED ? "ПРЕРВАНО ПО ЗАПРОСУ" : "ОШИБКА");
    dprintf(out_fd, "  Тактов работы:               %u\n", c->current_tick);
    dprintf(out_fd, "  Прибыло посылок:             %u\n", c->total_arrived);
    dprintf(out_fd, "  Доставлено в накопители:     %u (%.1f%%)\n",
            c->total_delivered,
            c->total_arrived > 0 ? (100.0 * c->total_delivered / c->total_arrived) : 0.0);
    dprintf(out_fd, "  Отбраковано (ручная зона):   %u (%.1f%%)\n",
            c->total_manual_rejected,
            c->total_arrived > 0 ? (100.0 * c->total_manual_rejected / c->total_arrived) : 0.0);
    dprintf(out_fd, "  Повторных сканирований:      %" PRIu64 "\n", c->total_rescans_performed);
    dprintf(out_fd, "  Событий ожидания за такты:   %" PRIu64 "\n", c->total_stalls_experienced);
    
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
    dprintf(out_fd, "  Отгружено с выходов: %u; передано оператору: %u\n", c->total_shipped, c->total_manual_serviced);
    dprintf(out_fd, "  Незавершённых посылок: %u\n", c->total_arrived - c->total_delivered - c->total_manual_rejected);
    dprintf(out_fd, "=============================================================\n\n");
}