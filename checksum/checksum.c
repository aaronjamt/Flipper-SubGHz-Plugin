#include <flipper_application/flipper_application.h>
#include "../layer.h"

#include <furi.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int send(void *storage, Buffer input, Buffer* output) {
    UNUSED(storage);
    // Output buffer will be 2 bytes larger than input buffer
    output->size = input.size + 2;
    output->data = (uint8_t*)malloc(output->size);

    // Calculate checksum
    uint16_t checksum = 0;
    for (size_t i = 0; i < input.size; i++) {
        checksum += input.data[i];
    }

    // Copy input data
    memcpy(output->data, input.data, input.size);

    // Add checksum to output
    output->data[input.size] = (checksum >> 8) & 0xFF; // High byte
    output->data[input.size + 1] = checksum & 0xFF; // Low byte

    // The full input buffer was processed
    return input.size;
}

int recv(void *storage, Buffer input, Buffer* output) {
    UNUSED(storage);

    if (input.size < 3) {
        // Not enough data for a checksum and data, return 0 to indicate no data processed
        return 0;
    }

    // Extract checksum value
    uint16_t checksum = (input.data[input.size - 2] << 8) | input.data[input.size - 1];

    // Validate checksum
    for (size_t i = 0; i < input.size - 2; i++) {
        checksum -= input.data[i];
    }

    if (checksum != 0) {
        // Checksum does not match, return input.size to discard input
        FURI_LOG_W("ChecksumLayer", "Checksum mismatch: expected 0, got %d\n", checksum);
        return input.size;
    }

    // Checksum is valid, copy data to output
    output->size = input.size - 2;
    output->data = (uint8_t*)malloc(output->size);
    memcpy(output->data, input.data, output->size);

    return input.size;
}

DataLayer *init() {
    DataLayer *layer = malloc(sizeof(DataLayer));
    memset(layer, 0, sizeof(DataLayer));
    layer->send = &send;
    layer->recv = &recv;
    layer->storage = (void*)0x1337c0de;

    return layer;
}



static const DataLayerEntryPoint data_layer_entry_point = {
    .init = &init
};

/* Plugin descriptor to comply with basic plugin specification */
static const FlipperAppPluginDescriptor layer_descriptor = {
    .appid = LAYER_APP_ID,
    .ep_api_version = LAYER_API_VERSION,
    .entry_point = &data_layer_entry_point,
};

/* Plugin entry point - must return a pointer to const descriptor */
const FlipperAppPluginDescriptor* layer_ep(void) {
    return &layer_descriptor;
}