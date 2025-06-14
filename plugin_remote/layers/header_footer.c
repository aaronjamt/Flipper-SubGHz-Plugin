// #include "header_footer.h"
#include "../layer.h"

#include <flipper_application/flipper_application.h>
#include <stdlib.h>
#include <string.h>

// Internal structure for plugin configuration
typedef struct {
    // Keep the configuration function pointer as the first member
    // This allows us to use the storage pointer as a function pointer
    //   to configure the layer from other code.
    void (*configure)(void *storage, char *header, size_t header_size, char *footer, size_t footer_size);

    // Store the actual header and footer data after the function pointer
    uint8_t *header;
    size_t header_size;
    uint8_t *footer;
    size_t footer_size;
} HeaderFooterConfig;

int send(void *storage, Buffer input, Buffer* output) {
    HeaderFooterConfig *config = (HeaderFooterConfig*)storage;

    // Calculate output buffer size (add header, footer, length byte)
    output->size = input.size+config->header_size+config->footer_size+1;
    output->data = (uint8_t*)malloc(output->size);

    // Copy header
    memcpy(output->data, config->header, config->header_size);
    // Add length byte
    output->data[config->header_size] = input.size;
    // Copy input data
    memcpy(output->data+config->header_size+1, input.data, input.size);
    // Copy footer
    memcpy(output->data+config->header_size+1+input.size, config->footer, config->footer_size);

    // The full input buffer was processed
    return input.size;
}

int recv(void *storage, Buffer input, Buffer* output) {
    HeaderFooterConfig *config = (HeaderFooterConfig*)storage;

    if (input.size < config->header_size + config->footer_size + 1) {
        // Not enough data for a full message, return 0 to indicate no data processed
        return 0;
    }

    for (size_t i = 0; i < input.size - config->header_size - config->footer_size; i++) {
        if (memcmp(input.data+i, config->header, config->header_size) == 0) {
            int header_start = i;
            i += config->header_size;
            uint8_t dataLength = input.data[i++];

            if (dataLength > input.size - i - config->footer_size) {
                // Not enough data for a full message, return the position of the header
                //   to discard any data before it.
                return header_start;
            }

            // Found a header, now look for the footer
            if (memcmp(input.data+i+dataLength, config->footer, config->footer_size) == 0) {
                // Valid header and footer, extract body
                output->size = dataLength;
                output->data = (uint8_t*)malloc(dataLength);
                memcpy(output->data, input.data + i, dataLength);

                // Return the number of input bytes processed
                return i + dataLength + config->footer_size;
            } else {
                // Invalid footer. Since we found a header,
                // return the number of bytes processed,
                // even though the output is empty.
                return i;
            }
        }
    }

    // No header found, return 0 to indicate no data was processed
    return 0;
}

void configure(void *storage, char *header, size_t header_size, char *footer, size_t footer_size) {
    HeaderFooterConfig *config = (HeaderFooterConfig*)storage;
    
    config->header = malloc(header_size);
    memcpy(config->header, header, header_size);
    config->header_size = header_size;

    config->footer = malloc(header_size);
    memcpy(config->footer, footer, footer_size);
    config->footer_size = footer_size;
}

DataLayer *init() {
    DataLayer *layer = malloc(sizeof(DataLayer));
    memset(layer, 0, sizeof(DataLayer));
    layer->send = &send;
    layer->recv = &recv;

    layer->storage = malloc(sizeof(HeaderFooterConfig));
    memset(layer->storage, 0, sizeof(HeaderFooterConfig));
    HeaderFooterConfig *config = (HeaderFooterConfig*)layer->storage;
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