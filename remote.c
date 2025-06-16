#include <flipper_application/plugins/plugin_manager.h>
#include <loader/firmware_api/firmware_api.h>
#include <lib/subghz/subghz_tx_rx_worker.h>

#include <string.h>
#include <stdlib.h>

#include "helpers/radio_device_loader.h"
#include "remote.h"
#include "layers/layer.h"

#define TAG "SubGHzRemotePlugin"

const SubGhzDevice* device;
SubGhzTxRxWorker* subghz_txrx;
bool is_suppressing_charge = false;

PluginManager* manager;

RemoteCallback rx_callback;
void* rx_callback_context;

DataLayer* layers_head = NULL;
DataLayer* layers_tail = NULL;

void free_buffer(Buffer* buffer) {
    furi_check(buffer);
    
    if (buffer->data != NULL) {
        free(buffer->data);
        buffer->data = NULL;
    }
    buffer->size = 0;
}

void append_buffer(Buffer *buffer, const uint8_t* data, size_t size) {
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

void append_layer(DataLayer *layer) {
    furi_check(layer);

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

bool remote_load_layer(const char *name, void **storage) {
    if (manager == NULL) {
        FURI_LOG_E(TAG, "Plugin was not initialized");
        return false;
    }
    if (name == NULL) {
        FURI_LOG_E(TAG, "Layer name cannot be NULL");
        return false;
    }
    
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
    furi_check(callback);

    rx_callback = callback;
    rx_callback_context = context;
}

void rx_event_callback(void* ctx) {
    UNUSED(ctx);

    FURI_LOG_D("PPNRM", "RX saw smth!");

    Buffer payload = {
        .data = NULL,
        .size = 0
    };

    // Read data from Sub-GHz until there's no more available
    int len;
    do {
        uint8_t buffer[32];
        len = (int)subghz_tx_rx_worker_read(subghz_txrx, buffer, sizeof(buffer));
        append_buffer(&payload, buffer, len);
    } while (len);

    // Pass the data through the layers in reverse order
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

    if (rx_callback == NULL) {
        FURI_LOG_W(TAG, "No RX callback set, dropping data!\n");
    } else {
        rx_callback(rx_callback_context, payload.data, payload.size);
    }
}

void remote_write(uint8_t* data, size_t len) {
    if (!subghz_txrx) {
        FURI_LOG_E(TAG, "SubGhz TX/RX worker is not running!");
        return;
    }

    // Allow sending 0 bytes, which will just trigger each layer's `send()` method
    //   without adding any new data.
    Buffer payload = {
        .data = NULL,
        .size = len
    };

    if (len != 0) {
        furi_check(data);
        payload.data = malloc(len);
        memcpy(payload.data, data, len);
    }

    // Pass the data through the layers
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

    // Transmit final payload
    while(!subghz_tx_rx_worker_write(subghz_txrx, payload.data, payload.size)) {
        // Wait a few milliseconds on failure before trying to send again.
        furi_delay_ms(7);
    }
    // Wait 10ms between each transmission to make sure they aren't received clumped together
    furi_delay_ms(100);
}

void remote_stop() {
    if(subghz_txrx != NULL) {
        if(subghz_tx_rx_worker_is_running(subghz_txrx)) {
            subghz_tx_rx_worker_stop(subghz_txrx);
        }
        subghz_tx_rx_worker_free(subghz_txrx);
        subghz_txrx = NULL;

        subghz_devices_deinit();
    }
}

bool remote_start(uint32_t frequency) {
    remote_stop();
    subghz_txrx = subghz_tx_rx_worker_alloc();
    subghz_devices_init();

    // All the SubGhz CLI apps disable charging so this plugin does too
    if(!is_suppressing_charge) {
        furi_hal_power_suppress_charge_enter();
        is_suppressing_charge = true;
    }

    // Request an external CC1101 antenna (will fall back to internal if unavailable)
    device = radio_device_loader_set(NULL, SubGhzRadioDeviceTypeExternalCC1101);

    subghz_devices_reset(device);
    subghz_devices_idle(device);

    if(!furi_hal_region_is_frequency_allowed(frequency)) {
        FURI_LOG_E(TAG, "Frequency not allowed: %ld.", frequency);
        return false;
    }

    furi_assert(device);
    if(subghz_tx_rx_worker_start(subghz_txrx, device, frequency)) {
        subghz_tx_rx_worker_set_callback_have_read(subghz_txrx, rx_event_callback, subghz_txrx);

        return true;
    } else {
        if(subghz_tx_rx_worker_is_running(subghz_txrx)) {
            subghz_tx_rx_worker_stop(subghz_txrx);
        }
        FURI_LOG_E(TAG, "Failed to start SubGhz TX/RX worker.");
        return false;
    }
}

bool remote_init() {
    // Prepare the plugin manager for loading layers
    manager = plugin_manager_alloc(LAYER_APP_ID, LAYER_API_VERSION, firmware_api_interface);

    return true;
}

void remote_free() {
    remote_stop();

    while (layers_head) {
        free(layers_head->storage);
        layers_head = layers_head->next_layer;
    }
    layers_head = NULL;
    layers_tail = NULL;

    if(is_suppressing_charge) {
        furi_hal_power_suppress_charge_exit();
        is_suppressing_charge = false;
    }

    if (manager != NULL) {
        plugin_manager_free(manager);
    }
}