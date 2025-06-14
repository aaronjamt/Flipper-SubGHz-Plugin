#pragma once

#include <stdint.h>

// Type for the config function
typedef struct {
    void (*configure)(void *storage, uint8_t frame_start, uint8_t frame_end, uint8_t frame_escape, uint8_t transposed_frame_start, uint8_t transposed_frame_end, uint8_t transposed_frame_escape);
} SLIPStorage;