#include <flipper_application/plugins/plugin_manager.h>
#include <loader/firmware_api/firmware_api.h>

#include <string.h>
#include <stdlib.h>

#include "remote.h"
#include "layer.h"

PluginManager* manager;

RemoteCallback rx_callback;
void* rx_callback_context;

DataLayer* layers_head = NULL;
DataLayer* layers_tail = NULL;

void free_buffer(Buffer* buffer) {
    if (buffer->data != NULL) {
        free(buffer->data);
        buffer->data = NULL;
    }
    buffer->size = 0;
}

void append_layer(DataLayer *layer) {
    if (layers_tail == NULL) {
        if (layers_head == NULL) {
            // This is (currently) the only layer
            layers_head = layers_tail = layer;
            layer->next_layer = layer->prev_layer = NULL;
            return;
        } else {
            // This should never be possible
            FURI_LOG_E(TAG, "Layers has null tail but non-null head! Please report at https://github.com/aaronjamt/Flipper-SubGHz-Plugin/issues/new\n");

            // Walk list to end to recover
            layers_tail = layers_head;
            while (layers_tail->next_layer != NULL)
                layers_tail = layers_tail->next_layer;
            
            // Fall through to append below
        }
    }

    // Append to end of list
    layers_tail->next_layer = layer;
    layer->prev_layer = layers_tail;
    layer->next_layer = NULL;
    layers_tail = layer;
}

bool load_layer(const char *name, void **storage) {
    // Concatinate the base plugins directory with the provided layer name
    const char *base_path = APP_ASSETS_PATH("plugins/");
    char *_path = malloc(strlen(base_path) + strlen(name) + 1);
    strcpy(_path, base_path);
    strcat(_path, name);

    uint32_t plugin_count = plugin_manager_get_count(manager);
    if (plugin_manager_load_single(manager, _path) == PluginManagerErrorNone) {
        // Make sure exactly one more plugin has been loaded
        if (plugin_manager_get_count(manager) == plugin_count + 1) {
            // Fetch and call entry point's init() method to create the DataLayer*
            DataLayerEntryPoint *ep = (DataLayerEntryPoint*)plugin_manager_get_ep(manager, plugin_count);
            DataLayer *layer = ep->init();
            
            // Add the DataLayer* to the list of layers
            append_layer(layer);

            // Share the DataLayer's layer-specific storage if a void** was provided
            if (storage != NULL) {
                *storage = layer->storage;
            }

            return true;
        }
    }

    return false;
}

void remote_set_rx_cb(RemoteCallback callback, void* context) {
    rx_callback = callback;
    rx_callback_context = context;
}

void rx_event_callback(Buffer payload) {
    DataLayer *layer = layers_tail;
    while (layer) {
        // Each layer contains a `recv_buffer` to store data that hasn't
        // yet been processed. Append the new payload to this buffer.
        layer->recv_buffer.data = realloc(layer->recv_buffer.data, layer->recv_buffer.size + payload.size);
        memcpy(layer->recv_buffer.data + layer->recv_buffer.size, payload.data, payload.size);
        layer->recv_buffer.size += payload.size;

        // Free the payload data as it has been copied into the layer's buffer
        free_buffer(&payload);

        // Now attempt to process the updated `recv_buffer`.
        int processed = layer->recv(layer->storage, layer->recv_buffer, &payload);

        // If the layer fails, try again once we get more data
        if (processed == 0) return;
        else {
            // We're going to remove the processed data, so update the buffer's size
            layer->recv_buffer.size -= processed;

            // If the buffer is now empty, free it
            if (layer->recv_buffer.size == 0) {
                free_buffer(&layer->recv_buffer);
            }
            // Otherwise, move the unprocessed data to the beginning of the buffer
            else {
                memmove(layer->recv_buffer.data, layer->recv_buffer.data + processed, layer->recv_buffer.size);
                layer->recv_buffer.data = realloc(layer->recv_buffer.data, layer->recv_buffer.size);
            }
        }
        // Move to the previous layer (so we reverse the order of operations from sending)
        layer = layer->prev_layer;
    }

    if (payload.size == 0) {
        FURI_LOG_W(TAG, "No data to send to callback!\n");
        return;
    }

    if (rx_callback == NULL)
        FURI_LOG_W(TAG, "No RX callback set, dropping data!\n");
    else
        rx_callback(rx_callback_context, payload.data, payload.size);
}

void remote_write(uint8_t* data, size_t len) {
    // Process layers
    Buffer payload = {
        .data = malloc(len),
        .size = len
    };
    memcpy(payload.data, data, len);

    DataLayer *layer = layers_head;
    while (layer) {
        Buffer output = {};
        size_t processed = layer->send(layer->storage, payload, &output);
        if (processed == 0) {
            // If the layer didn't process any data, skip it
            layer = layer->next_layer;
            continue;
        }

        free(payload.data);
        payload = output;
        layer = layer->next_layer;
    }

    // TODO: Send data to Sub-GHz worker
    rx_event_callback(payload);
}

void remote_free() {
    plugin_manager_free(manager);
}

bool remote_init() {
    manager = plugin_manager_alloc(LAYER_APP_ID, LAYER_API_VERSION, firmware_api_interface);
    return true;
}
