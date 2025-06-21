#include "layer.h"

#include <furi.h>

void buffer_free(Buffer* buffer) {
    furi_check(buffer);
    
    if (buffer->data != NULL) {
        free(buffer->data);
        buffer->data = NULL;
    }
    buffer->size = 0;
}

void buffer_append(Buffer *buffer, const uint8_t* data, size_t size) {
    furi_check(buffer);
    if (data == NULL || size == 0) return;

    if (buffer->data == NULL) {
        buffer->data = malloc(size);
        memcpy(buffer->data, data, size);
        buffer->size = size;
    } else {
        buffer->data = realloc(buffer->data, buffer->size + size);
        memcpy(buffer->data + buffer->size, data, size);
        buffer->size += size;
    }
}