// #include "slip.h"
#include "../layer.h"

#include <flipper_application/flipper_application.h>
#include <stdlib.h>
#include <string.h>

// Internal structure for plugin configuration
typedef struct {
    // Keep the configuration function pointer as the first member
    // This allows us to use the storage pointer as a function pointer
    //   to configure the layer from other code.
    void (*configure)(void *storage, uint8_t frame_start, uint8_t frame_end, uint8_t frame_escape, uint8_t transposed_frame_start, uint8_t transposed_frame_end, uint8_t transposed_frame_escape);

    // Store the actual values after the function pointer
    uint8_t frame_end;
    uint8_t frame_start;
    uint8_t frame_escape;
    uint8_t transposed_frame_end;
    uint8_t transposed_frame_start;
    uint8_t transposed_frame_escape;
} SLIPConfig;

int send(void *storage, Buffer input, Buffer* output) {
    SLIPConfig *config = (SLIPConfig*)storage;

    // The output might be up to twice the size of the input, plus
    //   2 bytes for the header and footer.
    output->size = input.size * 2 + 2;
    output->data = (uint8_t*)malloc(output->size);

    // Add the actual data
    size_t output_index = 0;
    output->data[output_index++] = config->frame_start;
    for (size_t i = 0; i < input.size; i++) {
        if (input.data[i] == config->frame_start) {
            // Escape the frame start
            output->data[output_index++] = config->frame_escape;
            output->data[output_index++] = config->transposed_frame_start;
        } else if (input.data[i] == config->frame_end) {
            // Escape the frame end
            output->data[output_index++] = config->frame_escape;
            output->data[output_index++] = config->transposed_frame_end;
        } else if (input.data[i] == config->frame_escape) {
            // Escape the escape character
            output->data[output_index++] = config->frame_escape;
            output->data[output_index++] = config->transposed_frame_escape;
        } else {
            // Normal byte, just copy it
            output->data[output_index++] = input.data[i];
        }
    }
    output->data[output_index++] = config->frame_end; // End of frame

    // Shrink the output buffer to the actual size
    if (output_index < output->size) {
        output->data = realloc(output->data, output_index);
    }
    output->size = output_index;

    // The full input buffer was processed
    return input.size;
}

int recv(void *storage, Buffer input, Buffer* output) {
    SLIPConfig *config = (SLIPConfig*)storage;

    if (input.size < 2) return 0; // Not enough data to process

    // Search for the SOF
    size_t sof_index = 0;
    for (size_t i=0; i<input.size; i++) {
        if (input.data[i] == config->frame_start) {
            sof_index = i;
            break;
        }
    }
    if (sof_index == 0 && input.data[0] != config->frame_start) {
        // No SOF found, return input.size to discard the input buffer
        FURI_LOG_W("SLIPLayer", "No SOF found\n");
        return input.size;
    }

    // Search for the EOF
    size_t eof_index = 0;
    for (size_t i=sof_index+1; i<input.size; i++) {
        if (input.data[i] == config->frame_end) {
            eof_index = i;
            break;
        }
    }
    if (eof_index == 0 && input.data[0] != config->frame_end) {
        // No SOF found, return 0 to indicate no data processed
        FURI_LOG_W("SLIPLayer", "No EOF found\n");
        return 0;
    }

    // SOF and EOF found, extract the data
    size_t data_size = eof_index - sof_index - 1; // Exclude SOF and EOF
    if (data_size == 0) {
        // No data between SOF and EOF, return EOF index to discard up to the end of the frame
        FURI_LOG_W("SLIPLayer", "No data between SOF and EOF\n");
        return eof_index+1;
    }

    // Allocate output buffer
    output->size = data_size;
    output->data = (uint8_t*)malloc(output->size);

    // Unescape any escaped bytes
    size_t output_index = 0;
    size_t input_index = sof_index + 1; // Start after SOF
    while (input_index < eof_index) {
        if (input.data[input_index] == config->frame_escape) {
            // Next byte is escaped
            input_index++;
            if (input.data[input_index] == config->transposed_frame_start) {
                output->data[output_index++] = config->frame_start;
            } else if (input.data[input_index] == config->transposed_frame_end) {
                output->data[output_index++] = config->frame_end;
            } else if (input.data[input_index] == config->transposed_frame_escape) {
                output->data[output_index++] = config->frame_escape;
            } else {
                // Unknown escape sequence, discard the frame
                FURI_LOG_W("SLIPLayer", "Unknown escape sequence: 0x%02X\n", input.data[input_index + 1]);
                output->size = 0;
                free(output->data);
                return eof_index+1;
            }
            input_index++;
        } else {
            output->data[output_index++] = input.data[input_index++];
        }
    }
    
    // Shrink output buffer to actual size
    if (output_index < output->size) {
        output->data = realloc(output->data, output_index);
        output->size = output_index;
    }

    return eof_index+1;
}

void configure(void *storage, uint8_t frame_start, uint8_t frame_end, uint8_t frame_escape, uint8_t transposed_frame_start, uint8_t transposed_frame_end, uint8_t transposed_frame_escape) {
    // Write the actual data into the object
    SLIPConfig *config = (SLIPConfig*)storage;

    config->frame_end = frame_end;
    config->frame_start = frame_start;
    config->frame_escape = frame_escape;
    config->transposed_frame_end = transposed_frame_end;
    config->transposed_frame_start = transposed_frame_start;
    config->transposed_frame_escape = transposed_frame_escape;
}

DataLayer *init() {
    DataLayer *layer = malloc(sizeof(DataLayer));
    memset(layer, 0, sizeof(DataLayer));
    layer->send = &send;
    layer->recv = &recv;

    layer->storage = malloc(sizeof(SLIPConfig));
    memset(layer->storage, 0, sizeof(SLIPConfig));
    SLIPConfig *config = (SLIPConfig*)layer->storage;
    config->configure = &configure;

    return layer;
}

// --------------------------------------------------------------------------------


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