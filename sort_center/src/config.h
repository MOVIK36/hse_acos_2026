#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Параметры моделирования
typedef struct {
    uint32_t num_parcels;              // Всего посылок
    uint32_t num_destinations;         // Число направлений
    uint32_t num_input_lines;          // Число входных линий
    uint32_t num_scanners;             // Число сканеров
    uint32_t num_sorters;              // Число сортировщиков

    uint32_t conveyor_length;          // Длина конвейера
    uint32_t conveyor_speed;           // Ячеек за такт (1..длина конвейера)

    uint32_t input_buffer_cap;         // Вместимость входного буфера
    uint32_t intermediate_buffer_cap;  // Вместимость буфера перед сортировкой
    uint32_t output_bin_cap;           // Вместимость накопителя выхода
    uint32_t manual_buffer_cap;        // Вместимость зоны брака
    uint32_t output_dispatch_interval; // Отгрузка выходных накопителей, такты
    uint32_t manual_service_interval;  // Освобождение ручной зоны, такты

    uint32_t arrival_interval_min;     // Мин. интервал между посылками
    uint32_t arrival_interval_max;     // Макс. интервал между посылками

    uint32_t scan_duration;            // Время сканирования (тактов)
    uint32_t sort_duration;            // Время сортировки (тактов)

    uint32_t label_error_prob_pct;     // Вероятность сбоя штрихкода (%)
    uint32_t max_rescan_attempts;      // Лимит попыток рескана

    uint32_t step_delay_ms;            // Длительность условного такта (мс; 0 = 1 мс)
    uint32_t random_seed;              // Начальное значение генераторов
    bool unlimited_mode;               // Бесконечный режим (до Ctrl+C)

    char log_filepath[256];            // Путь к файлу лога
    bool verbose_stdout;               // Печать событий в консоль
} sim_config_t;

void config_set_defaults(sim_config_t *cfg);
int config_validate(const sim_config_t *cfg);
int config_parse_args(sim_config_t *cfg, int argc, char *argv[]);
int config_load_file(sim_config_t *cfg, const char *filepath);
void config_print(const sim_config_t *cfg, int out_fd);

#endif /* CONFIG_H */
