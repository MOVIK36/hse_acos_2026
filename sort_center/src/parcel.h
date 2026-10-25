#ifndef PARCEL_H
#define PARCEL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Состояние штрихкода
typedef enum {
    LABEL_OK = 0,          // Штрихкод читается
    LABEL_UNREADABLE = 1,  // Временная ошибка чтения (блик/грязь)
    LABEL_DAMAGED = 2      // Нечитаемый штрихкод (брак)
} parcel_label_status_t;

// Этапы обработки посылки на конвейере
typedef enum {
    STAGE_CREATED = 0,
    STAGE_INPUT_BUFFER = 1,      // Во входном накопителе
    STAGE_SCANNING = 2,          // На сканере
    STAGE_POST_SCAN_BUFFER = 3,  // В промежуточном буфере
    STAGE_SORTING = 4,           // На сортировочном посту
    STAGE_CONVEYOR = 5,          // Едет по конвейеру
    STAGE_OUTPUT_BIN = 6,        // В выходном накопителе (успех)
    STAGE_MANUAL_INSPECTION = 7  // В ручной обработке (отбраковка)
} parcel_stage_t;

typedef struct {
    uint32_t id;                     // ID посылки
    uint32_t source_line;            // Линия прибытия (0..N-1)
    uint32_t target_destination;     // Направление (0..D-1)
    parcel_label_status_t label_stat;// Текущий статус штрихкода
    parcel_stage_t stage;            // Текущий этап

    uint32_t rescan_attempts;        // Сколько раз пробовали пересканировать
    uint32_t arrival_tick;           // Такт прибытия
    uint32_t completion_tick;        // Такт завершения

    bool destination_recognized;     // Распознано ли направление
} parcel_t;

const char *parcel_stage_to_string(parcel_stage_t stage);

#endif /* PARCEL_H */
