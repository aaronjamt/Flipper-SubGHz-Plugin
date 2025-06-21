#pragma once

#include <stdint.h>
#include <stdlib.h>

// The same APP_ID and API_VERSION are used in all
//   layers since the plugin manager will only load
//   layers with the same values as the main plugin.
#define LAYER_APP_ID "subghz_layer"
#define LAYER_API_VERSION 1

typedef struct {
    size_t size;
    uint8_t* data;
} Buffer;

typedef struct DataLayer {
    // These methods modify the data being passed in each direction. The input buffer
    //   is never modified, the output buffer will be updated with the result. Each
    //   returns the number of input bytes processed (i.e. 0 if no data was processed).
    int (*send)(void *storage, Buffer input, Buffer* output);
    int (*recv)(void *storage, Buffer input, Buffer* output);

    // These buffers are used to store data output from this layer that hasn't been
    //   processed yet (for example, when some, but not all, of a message has been
    //   received, and therefore can't be handled yet)
    Buffer send_buffer;
    Buffer recv_buffer;

    // This is generic storage for any layer-specific datatype(s) required.
    void *storage;

    // Double-linked list for chaining
    struct DataLayer *next_layer;
    struct DataLayer *prev_layer;
} DataLayer;

typedef struct {
    DataLayer *(*init)();
} DataLayerEntryPoint;

void buffer_free(Buffer* buffer);
void buffer_append(Buffer *buffer, const uint8_t* data, size_t size);