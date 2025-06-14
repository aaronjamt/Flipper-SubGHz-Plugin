#pragma once

#include <stdlib.h>

// Type for the config function
typedef struct {
    void (*configure)(void *storage, char *header, size_t header_size, char *footer, size_t footer_size);
} HeaderFooterStorage;

