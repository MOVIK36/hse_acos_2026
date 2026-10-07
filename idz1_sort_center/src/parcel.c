#include "parcel.h"

const char *parcel_stage_to_string(parcel_stage_t stage) {
    switch (stage) {
        case STAGE_CREATED:           return "CREATED";
        case STAGE_INPUT_BUFFER:      return "INPUT_BUF";
        case STAGE_SCANNING:          return "SCANNING";
        case STAGE_POST_SCAN_BUFFER:  return "INTERM_BUF";
        case STAGE_SORTING:           return "SORTING";
        case STAGE_CONVEYOR:          return "CONVEYOR";
        case STAGE_OUTPUT_BIN:        return "OUTPUT_BIN";
        case STAGE_MANUAL_INSPECTION: return "MANUAL_REJECT";
        default:                      return "UNKNOWN";
    }
}

const char *parcel_label_to_string(parcel_label_status_t status) {
    switch (status) {
        case LABEL_OK:         return "OK";
        case LABEL_UNREADABLE: return "UNREADABLE";
        case LABEL_DAMAGED:    return "DAMAGED";
        default:               return "UNKNOWN";
    }
}