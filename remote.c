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
Buffer subghz_tx_buffer;
FuriThread *tx_worker_thread;
FuriMutex *tx_worker_mutex;
bool is_suppressing_charge = false;

PluginManager* manager;

RemoteCallback rx_callback;
void* rx_callback_context;

DataLayer* layers_head = NULL;
DataLayer* layers_tail = NULL;

typedef enum {
    WorkerEventStop  = 1 << 1,
    WorkerEventData  = 1 << 2,
    WorkerEventFlush = 1 << 3,
    WorkerEventTimer = 1 << 4,
} WorkerEventFlags;

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

    Buffer payload = {
        .data = NULL,
        .size = 0
    };

    // Read data from Sub-GHz until there's no more available
    int len;
    do {
        uint8_t buffer[32];
        len = (int)subghz_tx_rx_worker_read(subghz_txrx, buffer, sizeof(buffer));
        buffer_append(&payload, buffer, len);
    } while (len);

    // Pass the data through the layers in reverse order
    DataLayer *layer = layers_tail;
    while (layer) {
        // Add any data from the payload buffer to the layer's receive buffer
        if (payload.size) {
            buffer_append(&layer->recv_buffer, payload.data, payload.size);
            buffer_free(&payload);
        }

        int processed = layer->recv(layer->storage, layer->recv_buffer, &payload);

        if (processed == 0) {
            // If the layer didn't process any data, skip updating the receive buffer
            layer = layer->prev_layer;
            continue;
        }

        // We're going to remove the processed data, so update the buffer's size
        layer->recv_buffer.size -= processed;

        if (layer->recv_buffer.size == 0) {
            // If the buffer is now empty, free it
            buffer_free(&layer->recv_buffer);
        } else {
            // Otherwise, move the unprocessed data to the beginning of the buffer
            memmove(layer->recv_buffer.data, layer->recv_buffer.data + processed, layer->recv_buffer.size);
            layer->recv_buffer.data = realloc(layer->recv_buffer.data, layer->recv_buffer.size);
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

void remote_write(uint8_t *data, size_t size) {
    if (!tx_worker_mutex || !tx_worker_thread) {
        FURI_LOG_E(TAG, "Sub-GHz TX/RX worker is not running!");
        furi_check(false);
    }

    if (furi_mutex_acquire(tx_worker_mutex, FuriWaitForever) != FuriStatusOk) {
        FURI_LOG_E(TAG, "Failed to write data: could not lock mutex!");
        furi_check(false);
    }

    buffer_append(&subghz_tx_buffer, data, size);
    furi_mutex_release(tx_worker_mutex);

    furi_thread_flags_set(furi_thread_get_id(tx_worker_thread), WorkerEventData);
}

static int32_t tx_worker(void* context) {
    UNUSED(context);

    tx_worker_mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    if (!subghz_txrx) {
        FURI_LOG_E(TAG, "Sub-GHz TX/RX worker is not running!");
        return 0;
    }

    while (1) {
        uint32_t events =
            furi_thread_flags_wait(WorkerEventStop | WorkerEventData, FuriFlagWaitAny | FuriFlagNoClear, 100);
        furi_check((events & FuriFlagError) == 0 || events == FuriFlagErrorTimeout);

        if(events & WorkerEventStop) break;

        Buffer payload = {
            .data = NULL,
            .size = 0
        };

        // Only read in new data if we can acquire the mutex
        if ((events & WorkerEventData) && (furi_mutex_acquire(tx_worker_mutex, 25) == FuriStatusOk)) {
            furi_thread_flags_clear(WorkerEventData);
            payload.size = subghz_tx_buffer.size;

            if (payload.size) {
                furi_check(subghz_tx_buffer.data);
                payload.data = malloc(payload.size);
                memcpy(payload.data, subghz_tx_buffer.data, payload.size);
            }

            buffer_free(&subghz_tx_buffer);
            furi_mutex_free(tx_worker_mutex);
        }
        
        // Pass the data through the layers
        DataLayer *layer = layers_head;
        while (layer) {
            // Add any data from the payload buffer to the layer's send buffer
            if (payload.size) {
                buffer_append(&layer->send_buffer, payload.data, payload.size);
                buffer_free(&payload);
            }

            size_t processed = layer->send(layer->storage, layer->send_buffer, &payload);

            if (processed == 0) {
                // If the layer didn't process any data, skip updating the send buffer
                layer = layer->next_layer;
                continue;
            }

            // We're going to remove the processed data, so update the buffer's size
            layer->send_buffer.size -= processed;

            // If the buffer is now empty, free it
            if (layer->send_buffer.size == 0) {
                buffer_free(&layer->send_buffer);
            }
            // Otherwise, move the unprocessed data to the beginning of the buffer
            else {
                memmove(layer->send_buffer.data, layer->send_buffer.data + processed, layer->send_buffer.size);
                layer->send_buffer.data = realloc(layer->send_buffer.data, layer->send_buffer.size);
            }

            layer = layer->next_layer;
        }

        if (payload.size) {
            // Transmit final payload
            while(!subghz_tx_rx_worker_write(subghz_txrx, payload.data, payload.size)) {
                // Wait a few milliseconds on failure before trying to send again
                furi_delay_ms(7);
            }
        }

        if (events & WorkerEventFlush) {
            // TODO: Figure out how to flush the SubGHz TX/RX worker's buffer
            furi_thread_flags_clear(WorkerEventFlush);
        }
    }

    furi_mutex_free(tx_worker_mutex);

    return 0;
}

void remote_stop() {
    if (tx_worker_thread != NULL) {
        furi_thread_flags_set(furi_thread_get_id(tx_worker_thread), WorkerEventStop);
        furi_thread_join(tx_worker_thread);
        furi_thread_free(tx_worker_thread);
        tx_worker_thread = NULL;
    }

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
    // Start TX/RX worker threads
    if(subghz_tx_rx_worker_start(subghz_txrx, device, frequency)) {
        subghz_tx_rx_worker_set_callback_have_read(subghz_txrx, rx_event_callback, subghz_txrx);

        tx_worker_thread = furi_thread_alloc_ex("SubGHzPluginTXWorker", 1024, tx_worker, NULL);
        furi_thread_start(tx_worker_thread);

        return true;
    } else {
        if(subghz_tx_rx_worker_is_running(subghz_txrx)) {
            subghz_tx_rx_worker_stop(subghz_txrx);
        }
        FURI_LOG_E(TAG, "Failed to start Sub-GHz TX/RX worker.");
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