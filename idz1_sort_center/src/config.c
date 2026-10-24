#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void config_set_defaults(sim_config_t *cfg) {
    *cfg = (sim_config_t){
        .num_parcels = 24, .num_destinations = 3, .num_input_lines = 2,
        .num_scanners = 2, .num_sorters = 1, .conveyor_length = 4, .conveyor_speed = 1,
        .input_buffer_cap = 6, .intermediate_buffer_cap = 4,
        .output_bin_cap = 20, .manual_buffer_cap = 10, .rescan_buffer_cap = 12,
        .output_dispatch_interval = 10, .manual_service_interval = 10,
        .arrival_interval_min = 1, .arrival_interval_max = 3,
        .scan_duration = 2, .sort_duration = 1, .label_error_prob_pct = 20,
        .max_rescan_attempts = 2, .step_delay_ms = 25, .random_seed = 42,
        .verbose_stdout = true, .log_filepath = "sort_center.log"
    };
}

static int parse_uint(const char *text, uint32_t *value) {
    if (!text || !isdigit((unsigned char)*text)) return -1;
    errno = 0;
    char *end;
    unsigned long long number = strtoull(text, &end, 10);
    if (errno || *end || number > UINT32_MAX) return -1;
    *value = (uint32_t)number;
    return 0;
}

static int set_parameter(sim_config_t *cfg, const char *name, const char *text) {
    if (strcmp(name, "log_filepath") == 0) {
        if (!*text || strlen(text) >= sizeof(cfg->log_filepath)) goto invalid;
        strcpy(cfg->log_filepath, text);
        return 0;
    }
    uint32_t value;
    if (parse_uint(text, &value) != 0) goto invalid;
    if (strcmp(name, "num_parcels") == 0) cfg->num_parcels = value;
    else if (strcmp(name, "num_destinations") == 0) cfg->num_destinations = value;
    else if (strcmp(name, "num_input_lines") == 0) cfg->num_input_lines = value;
    else if (strcmp(name, "num_scanners") == 0) cfg->num_scanners = value;
    else if (strcmp(name, "num_sorters") == 0) cfg->num_sorters = value;
    else if (strcmp(name, "conveyor_length") == 0) cfg->conveyor_length = value;
    else if (strcmp(name, "conveyor_speed") == 0) cfg->conveyor_speed = value;
    else if (strcmp(name, "input_buffer_cap") == 0) cfg->input_buffer_cap = value;
    else if (strcmp(name, "intermediate_buffer_cap") == 0) cfg->intermediate_buffer_cap = value;
    else if (strcmp(name, "output_bin_cap") == 0) cfg->output_bin_cap = value;
    else if (strcmp(name, "manual_buffer_cap") == 0) cfg->manual_buffer_cap = value;
    else if (strcmp(name, "rescan_buffer_cap") == 0) cfg->rescan_buffer_cap = value;
    else if (strcmp(name, "output_dispatch_interval") == 0) cfg->output_dispatch_interval = value;
    else if (strcmp(name, "manual_service_interval") == 0) cfg->manual_service_interval = value;
    else if (strcmp(name, "arrival_interval_min") == 0) cfg->arrival_interval_min = value;
    else if (strcmp(name, "arrival_interval_max") == 0) cfg->arrival_interval_max = value;
    else if (strcmp(name, "scan_duration") == 0) cfg->scan_duration = value;
    else if (strcmp(name, "sort_duration") == 0) cfg->sort_duration = value;
    else if (strcmp(name, "label_error_prob_pct") == 0) cfg->label_error_prob_pct = value;
    else if (strcmp(name, "max_rescan_attempts") == 0) cfg->max_rescan_attempts = value;
    else if (strcmp(name, "step_delay_ms") == 0) cfg->step_delay_ms = value;
    else if (strcmp(name, "random_seed") == 0) cfg->random_seed = value;
    else if (strcmp(name, "unlimited_mode") == 0 && value <= 1) cfg->unlimited_mode = value != 0;
    else if (strcmp(name, "verbose_stdout") == 0 && value <= 1) cfg->verbose_stdout = value != 0;
    else goto invalid;
    return 0;
invalid:
    dprintf(STDERR_FILENO, "[CONFIG] Неизвестный параметр или неверное значение: %s=%s\n", name, text);
    return -1;
}

int config_validate(const sim_config_t *c) {
    if (!c) return -1;
    if (!c->num_destinations || !c->num_input_lines || !c->num_scanners || !c->num_sorters ||
        c->num_destinations > 100 || c->num_input_lines > 100 || c->num_scanners > 100 || c->num_sorters > 100) {
        dprintf(STDERR_FILENO, "[CONFIG] Число направлений, линий и устройств должно быть от 1 до 100\n");
        return -1;
    }
    if (!c->conveyor_length || !c->input_buffer_cap || !c->intermediate_buffer_cap ||
        !c->output_bin_cap || !c->manual_buffer_cap || !c->rescan_buffer_cap ||
        c->conveyor_length > 10000 || c->input_buffer_cap > 10000 || c->intermediate_buffer_cap > 10000 ||
        c->output_bin_cap > 10000 || c->manual_buffer_cap > 10000 || c->rescan_buffer_cap > 10000) {
        dprintf(STDERR_FILENO, "[CONFIG] Длина ленты и вместимости должны быть от 1 до 10000\n");
        return -1;
    }
    if (!c->conveyor_speed || c->conveyor_speed > c->conveyor_length || c->label_error_prob_pct > 100 ||
        !c->arrival_interval_min || c->arrival_interval_min > c->arrival_interval_max ||
        !c->scan_duration || !c->sort_duration || !c->output_dispatch_interval || !c->manual_service_interval) {
        dprintf(STDERR_FILENO, "[CONFIG] Проверьте скорость, вероятность ошибки, интервалы и длительности\n");
        return -1;
    }
    if (c->num_parcels > 1000000 || c->arrival_interval_max > 1000000 || c->scan_duration > 1000000 ||
        c->sort_duration > 1000000 || c->output_dispatch_interval > 1000000 ||
        c->manual_service_interval > 1000000 || c->max_rescan_attempts > 1000000 || c->step_delay_ms > 60000) {
        dprintf(STDERR_FILENO, "[CONFIG] Слишком большое значение параметра (предел 1000000, задержка до 60000 мс)\n");
        return -1;
    }
    return 0;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) ++s;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) --end;
    *end = '\0';
    return s;
}

int config_load_file(sim_config_t *cfg, const char *filepath) {
    FILE *file = fopen(filepath, "r");
    if (!file) {
        dprintf(STDERR_FILENO, "[CONFIG] Не удалось прочитать '%s': %s\n", filepath, strerror(errno));
        return -1;
    }
    char line[1024];
    unsigned line_number = 0;
    int result = 0;
    while (fgets(line, sizeof(line), file)) {
        ++line_number;
        if (!strchr(line, '\n') && !feof(file)) { result = -1; break; }
        char *p = trim(line);
        if (!*p || *p == '#' || *p == ';') continue;
        char *eq = strchr(p, '=');
        if (!eq) { result = -1; break; }
        *eq = '\0';
        if (set_parameter(cfg, trim(p), trim(eq + 1)) != 0) { result = -1; break; }
    }
    if (ferror(file)) result = -1;
    if (result) dprintf(STDERR_FILENO, "[CONFIG] Ошибка в %s, строка %u\n", filepath, line_number);
    fclose(file);
    return result;
}

int config_parse_args(sim_config_t *cfg, int argc, char *argv[]) {
    static const struct option options[] = {
        {"parcels", required_argument, NULL, 'n'}, {"destinations", required_argument, NULL, 'd'},
        {"lines", required_argument, NULL, 'l'}, {"scanners", required_argument, NULL, 's'},
        {"sorters", required_argument, NULL, 'o'}, {"error-rate", required_argument, NULL, 'e'},
        {"rescan-max", required_argument, NULL, 'r'}, {"seed", required_argument, NULL, 'S'},
        {"delay", required_argument, NULL, 'D'}, {"config", required_argument, NULL, 'c'},
        {"log", required_argument, NULL, 'L'}, {"unlimited", no_argument, NULL, 'u'},
        {"quiet", no_argument, NULL, 'q'}, {"help", no_argument, NULL, 'h'},
        {"conveyor-length", required_argument, NULL, 1000},
        {"conveyor-speed", required_argument, NULL, 1001},
        {"output-interval", required_argument, NULL, 1002},
        {"manual-interval", required_argument, NULL, 1003},
        {"rescan-capacity", required_argument, NULL, 1004}, {NULL, 0, NULL, 0}
    };
    const char *short_options = "n:d:l:s:o:e:r:S:D:c:L:uqh";
    // Сначала файлы, затем все CLI-переопределения, независимо от позиции -c.
    opterr = 0;
    optind = 0;
    int option;
    while ((option = getopt_long(argc, argv, short_options, options, NULL)) != -1) {
        if (option == 'h') {
            dprintf(STDOUT_FILENO,
                "Использование: %s [параметры]\n"
                "  -n, --parcels N         Число посылок (0 — пустой запуск)\n"
                "  -d, --destinations N    Число направлений\n"
                "  -l, --lines N           Число входных линий\n"
                "  -s, --scanners N        Число сканеров\n"
                "  -o, --sorters N         Число сортировщиков\n"
                "  -e, --error-rate PCT    Вероятность ошибки, 0..100%%\n"
                "  -r, --rescan-max N      Число повторов после первого чтения\n"
                "  -S, --seed N            Начальное значение генератора\n"
                "  -D, --delay MS          Пауза между тактами, 0..60000 мс\n"
                "  -c, --config FILE       Конфигурация key=value\n"
                "  -L, --log FILE          Журнал событий\n"
                "  -u, --unlimited         Работать до SIGINT/SIGTERM\n"
                "  -q, --quiet             Только параметры и итоги в терминале\n"
                "      --conveyor-length N Длина ленты в ячейках\n"
                "      --conveyor-speed N  Скорость, ячеек/такт, 1..длина\n"
                "      --output-interval N Интервал отгрузки выходных накопителей\n"
                "      --manual-interval N Интервал освобождения ручной зоны\n"
                "      --rescan-capacity N Вместимость очереди повторов\n"
                "  -h, --help              Справка\n"
                "CLI имеет приоритет над файлами. Остальные параметры задаются в .conf.\n", argv[0]);
            return 1;
        }
        if (option == 'c' && config_load_file(cfg, optarg) != 0) return -1;
        if (option == '?') {
            dprintf(STDERR_FILENO, "[CONFIG] Неизвестный аргумент или пропущено значение\n");
            return -1;
        }
    }
    optind = 0;
    opterr = 1;
    while ((option = getopt_long(argc, argv, short_options, options, NULL)) != -1) {
        const char *key = NULL;
        switch (option) {
            case 'n': key = "num_parcels"; break;
            case 'd': key = "num_destinations"; break;
            case 'l': key = "num_input_lines"; break;
            case 's': key = "num_scanners"; break;
            case 'o': key = "num_sorters"; break;
            case 'e': key = "label_error_prob_pct"; break;
            case 'r': key = "max_rescan_attempts"; break;
            case 'S': key = "random_seed"; break;
            case 'D': key = "step_delay_ms"; break;
            case 'L': key = "log_filepath"; break;
            case 1000: key = "conveyor_length"; break;
            case 1001: key = "conveyor_speed"; break;
            case 1002: key = "output_dispatch_interval"; break;
            case 1003: key = "manual_service_interval"; break;
            case 1004: key = "rescan_buffer_cap"; break;
            case 'u': cfg->unlimited_mode = true; break;
            case 'q': cfg->verbose_stdout = false; break;
            case 'c': break;
            default: return -1;
        }
        if (key && set_parameter(cfg, key, optarg) != 0) return -1;
    }
    if (optind != argc) {
        dprintf(STDERR_FILENO, "[CONFIG] Лишний аргумент: %s\n", argv[optind]);
        return -1;
    }
    return config_validate(cfg);
}

void config_print(const sim_config_t *c, int fd) {
    dprintf(fd, "=== Вариант 34: Сортировочный центр ===\n");
    dprintf(fd, "Посылок: %u; направлений: %u; входных линий: %u\n",
            c->num_parcels, c->num_destinations, c->num_input_lines);
    dprintf(fd, "Сканеров: %u; сортировщиков: %u; время обработки: %u / %u тактов\n",
            c->num_scanners, c->num_sorters, c->scan_duration, c->sort_duration);
    dprintf(fd, "Конвейер: %u ячеек, скорость %u ячеек/такт\n", c->conveyor_length, c->conveyor_speed);
    dprintf(fd, "Вместимости: вход %u; промежуточный %u; выход %u; ручная зона %u; повторы %u\n",
            c->input_buffer_cap, c->intermediate_buffer_cap, c->output_bin_cap, c->manual_buffer_cap, c->rescan_buffer_cap);
    dprintf(fd, "Поступление: каждые %u..%u тактов; ошибка чтения %u%%; повторов до %u\n",
            c->arrival_interval_min, c->arrival_interval_max, c->label_error_prob_pct, c->max_rescan_attempts);
    dprintf(fd, "Отгрузка каждые %u тактов; освобождение ручной зоны каждые %u тактов\n",
            c->output_dispatch_interval, c->manual_service_interval);
    dprintf(fd, "Seed: %u; задержка: %u мс; неограниченный режим: %s; журнал: %s\n",
            c->random_seed, c->step_delay_ms, c->unlimited_mode ? "да" : "нет", c->log_filepath);
}
