#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <ctype.h>
#include <stddef.h>
#include "config.h"

void config_set_defaults(sim_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->num_parcels = 24;
    cfg->num_destinations = 3;
    cfg->num_input_lines = 2;
    cfg->num_scanners = 2;
    cfg->num_sorters = 1;
    
    cfg->conveyor_length = 4;
    cfg->conveyor_transit_ticks = 4;
    
    cfg->input_buffer_cap = 6;
    cfg->intermediate_buffer_cap = 4;
    cfg->output_bin_cap = 20;
    cfg->manual_buffer_cap = 10;
    
    cfg->arrival_interval_min = 1;
    cfg->arrival_interval_max = 3;
    
    cfg->scan_duration = 2;
    cfg->sort_duration = 1;
    
    cfg->label_error_prob_pct = 20;
    cfg->max_rescan_attempts = 2;
    
    cfg->step_delay_ms = 25;
    cfg->random_seed = 42;
    cfg->unlimited_mode = false;
    
    strncpy(cfg->log_filepath, "sort_center.log", sizeof(cfg->log_filepath) - 1);
    cfg->verbose_stdout = true;
}

static char *trim_whitespace(char *str) {
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

int config_load_file(sim_config_t *cfg, const char *filepath) {
    FILE *f = fopen(filepath, "r");
    if (!f) {
        dprintf(STDERR_FILENO, "[CONFIG] Не удалось прочитать конфиг '%s'\n", filepath);
        return -1;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim_whitespace(line);
        if (*p == '#' || *p == '\0' || *p == ';') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim_whitespace(p);
        char *val = trim_whitespace(eq + 1);
        
        if (strcmp(key, "num_parcels") == 0) cfg->num_parcels = (uint32_t)atoi(val);
        else if (strcmp(key, "num_destinations") == 0) cfg->num_destinations = (uint32_t)atoi(val);
        else if (strcmp(key, "num_input_lines") == 0) cfg->num_input_lines = (uint32_t)atoi(val);
        else if (strcmp(key, "num_scanners") == 0) cfg->num_scanners = (uint32_t)atoi(val);
        else if (strcmp(key, "num_sorters") == 0) cfg->num_sorters = (uint32_t)atoi(val);
        else if (strcmp(key, "conveyor_length") == 0) {
            cfg->conveyor_length = (uint32_t)atoi(val);
            cfg->conveyor_transit_ticks = cfg->conveyor_length;
        }
        else if (strcmp(key, "input_buffer_cap") == 0) cfg->input_buffer_cap = (uint32_t)atoi(val);
        else if (strcmp(key, "intermediate_buffer_cap") == 0) cfg->intermediate_buffer_cap = (uint32_t)atoi(val);
        else if (strcmp(key, "output_bin_cap") == 0) cfg->output_bin_cap = (uint32_t)atoi(val);
        else if (strcmp(key, "manual_buffer_cap") == 0) cfg->manual_buffer_cap = (uint32_t)atoi(val);
        else if (strcmp(key, "arrival_interval_min") == 0) cfg->arrival_interval_min = (uint32_t)atoi(val);
        else if (strcmp(key, "arrival_interval_max") == 0) cfg->arrival_interval_max = (uint32_t)atoi(val);
        else if (strcmp(key, "scan_duration") == 0) cfg->scan_duration = (uint32_t)atoi(val);
        else if (strcmp(key, "sort_duration") == 0) cfg->sort_duration = (uint32_t)atoi(val);
        else if (strcmp(key, "label_error_prob_pct") == 0) cfg->label_error_prob_pct = (uint32_t)atoi(val);
        else if (strcmp(key, "max_rescan_attempts") == 0) cfg->max_rescan_attempts = (uint32_t)atoi(val);
        else if (strcmp(key, "step_delay_ms") == 0) cfg->step_delay_ms = (uint32_t)atoi(val);
        else if (strcmp(key, "random_seed") == 0) cfg->random_seed = (uint32_t)atoi(val);
        else if (strcmp(key, "unlimited_mode") == 0) cfg->unlimited_mode = (atoi(val) != 0);
        else if (strcmp(key, "log_filepath") == 0) strncpy(cfg->log_filepath, val, sizeof(cfg->log_filepath) - 1);
    }
    fclose(f);
    return 0;
}

int config_parse_args(sim_config_t *cfg, int argc, char *argv[]) {
    static struct option long_options[] = {
        {"parcels",      required_argument, 0, 'n'},
        {"destinations", required_argument, 0, 'd'},
        {"lines",        required_argument, 0, 'l'},
        {"scanners",     required_argument, 0, 's'},
        {"sorters",      required_argument, 0, 'o'},
        {"error-rate",   required_argument, 0, 'e'},
        {"rescan-max",   required_argument, 0, 'r'},
        {"seed",         required_argument, 0, 'S'},
        {"delay",        required_argument, 0, 'D'},
        {"config",       required_argument, 0, 'c'},
        {"log",          required_argument, 0, 'L'},
        {"unlimited",    no_argument,       0, 'u'},
        {"quiet",        no_argument,       0, 'q'},
        {"help",         no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "n:d:l:s:o:e:r:S:D:c:L:uqh", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'n': cfg->num_parcels = (uint32_t)atoi(optarg); break;
            case 'd': cfg->num_destinations = (uint32_t)atoi(optarg); break;
            case 'l': cfg->num_input_lines = (uint32_t)atoi(optarg); break;
            case 's': cfg->num_scanners = (uint32_t)atoi(optarg); break;
            case 'o': cfg->num_sorters = (uint32_t)atoi(optarg); break;
            case 'e': cfg->label_error_prob_pct = (uint32_t)atoi(optarg); break;
            case 'r': cfg->max_rescan_attempts = (uint32_t)atoi(optarg); break;
            case 'S': cfg->random_seed = (uint32_t)atoi(optarg); break;
            case 'D': cfg->step_delay_ms = (uint32_t)atoi(optarg); break;
            case 'c':
                if (config_load_file(cfg, optarg) != 0) return -1;
                break;
            case 'L': strncpy(cfg->log_filepath, optarg, sizeof(cfg->log_filepath) - 1); break;
            case 'u': cfg->unlimited_mode = true; break;
            case 'q': cfg->verbose_stdout = false; break;
            case 'h':
                dprintf(STDOUT_FILENO,
                    "Использование: %s [параметры]\n"
                    "Параметры:\n"
                    "  -n, --parcels NUM       Число посылок (по умолчанию: %u)\n"
                    "  -d, --destinations NUM  Число направлений (по умолчанию: %u)\n"
                    "  -l, --lines NUM         Входных линий (по умолчанию: %u)\n"
                    "  -s, --scanners NUM      Число сканеров (по умолчанию: %u)\n"
                    "  -o, --sorters NUM       Число сортировщиков (по умолчанию: %u)\n"
                    "  -e, --error-rate PCT    Процент сбоев штрихкода (по умолчанию: %u%%)\n"
                    "  -r, --rescan-max NUM    Макс. число ресканов (по умолчанию: %u)\n"
                    "  -S, --seed NUM          Seed для генератора случайных чисел (%u)\n"
                    "  -D, --delay MS          Задержка между тактами в мс (%u)\n"
                    "  -c, --config FILE       Файл конфигурации\n"
                    "  -L, --log FILE          Файл для логов (по умолчанию: %s)\n"
                    "  -u, --unlimited         Режим до нажатия Ctrl+C\n"
                    "  -q, --quiet             Не выводить события в терминал\n"
                    "  -h, --help              Справка\n",
                    argv[0], cfg->num_parcels, cfg->num_destinations, cfg->num_input_lines,
                    cfg->num_scanners, cfg->num_sorters, cfg->label_error_prob_pct,
                    cfg->max_rescan_attempts, cfg->random_seed, cfg->step_delay_ms,
                    cfg->log_filepath);
                return 1;
            default:
                return -1;
        }
    }
    return 0;
}

void config_print(const sim_config_t *cfg, int out_fd) {
    dprintf(out_fd, "=== Параметры симуляции (Вариант 34: Сортировочный центр) ===\n");
    dprintf(out_fd, "  Посылок:                 %u %s\n", cfg->num_parcels, cfg->unlimited_mode ? "(unlimited)" : "");
    dprintf(out_fd, "  Входных линий:           %u (емкость буфера: %u)\n", cfg->num_input_lines, cfg->input_buffer_cap);
    dprintf(out_fd, "  Сканеров:                %u (время сканирования: %u)\n", cfg->num_scanners, cfg->scan_duration);
    dprintf(out_fd, "  Промежуточный буфер:     емкость %u\n", cfg->intermediate_buffer_cap);
    dprintf(out_fd, "  Сортировщиков:           %u (время сортировки: %u)\n", cfg->num_sorters, cfg->sort_duration);
    dprintf(out_fd, "  Направлений:             %u (лента: %u, накопитель: %u)\n",
            cfg->num_destinations, cfg->conveyor_length, cfg->output_bin_cap);
    dprintf(out_fd, "  Буфер брака:             емкость %u\n", cfg->manual_buffer_cap);
    dprintf(out_fd, "  Вероятность сбоя кода:   %u%% (макс. попыток: %u)\n", cfg->label_error_prob_pct, cfg->max_rescan_attempts);
    dprintf(out_fd, "  Seed:                    %u, Задержка: %u мс, Лог: %s\n", cfg->random_seed, cfg->step_delay_ms, cfg->log_filepath);
    dprintf(out_fd, "=============================================================\n\n");
}