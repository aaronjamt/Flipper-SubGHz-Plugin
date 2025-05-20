
#include "interface.h"
#include <flipper_application/flipper_application.h>

#include <furi_hal.h>

#include <lib/subghz/subghz_tx_rx_worker.h>
#include "helpers/radio_device_loader.h"

#define _TAG "PicoPassRemote"

const SubGhzDevice* device;
SubGhzTxRxWorker* subghz_txrx;

RemoteCallback rx_callback;
void* rx_callback_context;

// Maximum length of a single message (in bytes)
const uint8_t MESSAGE_MAX_LEN = 32;

// Don't use 433.920MHz as it's a harmonic frequency of the 13.56MHz signal used for the card
// const uint32_t FREQUENCY = 433420000;
const uint32_t FREQUENCY = 433560000;

bool is_suppressing_charge = false;

void rx_event_callback(void* ctx) {
    UNUSED(ctx);

    uint8_t buffer[MESSAGE_MAX_LEN];

    // The message must be for us, read the rest of it and pass it to the callback
    int len = (int)subghz_tx_rx_worker_read(subghz_txrx, buffer, MESSAGE_MAX_LEN);
    rx_callback(rx_callback_context, buffer, len);
}

void remote_free() {
    if(is_suppressing_charge) {
        furi_hal_power_suppress_charge_exit();
        is_suppressing_charge = false;
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

bool remote_init() {
    if(subghz_txrx != NULL) {
        remote_free();
    }
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

    // TODO: This crashes when using external CC1101
    // if (!subghz_devices_is_connect(device)) {
    //     return false;
    // }

    if(!furi_hal_region_is_frequency_allowed(FREQUENCY)) {
        FURI_LOG_E(_TAG, "Frequency not allowed %ld.", FREQUENCY);
        return false;
    }

    furi_assert(device);
    if(subghz_tx_rx_worker_start(subghz_txrx, device, FREQUENCY)) {
        subghz_tx_rx_worker_set_callback_have_read(subghz_txrx, rx_event_callback, subghz_txrx);
    } else {
        if(subghz_tx_rx_worker_is_running(subghz_txrx)) {
            subghz_tx_rx_worker_stop(subghz_txrx);
        }
        FURI_LOG_E(_TAG, "Failed to start SubGhz TX/RX worker.");
        return false;
    }

    return true;
}

void remote_write(uint8_t* data, size_t len) {
    while(!subghz_tx_rx_worker_write(subghz_txrx, data, len)) {
        // Wait a few milliseconds on failure before trying to send again.
        furi_delay_ms(7);
    }
    // Wait 10ms between each transmission to make sure they aren't received clumped together
    furi_delay_ms(10);
}

void remote_set_rx_cb(RemoteCallback callback, void* context) {
    rx_callback = callback;
    rx_callback_context = context;
}

/* Actual implementation of app<>plugin interface */
static const PluginRemote plugin_remote = {
    .name = "Plugin Remote",
    .init = &remote_init,
    .free = &remote_free,
    .write = &remote_write,
    .set_rx_cb = &remote_set_rx_cb,
};

/* Plugin descriptor to comply with basic plugin specification */
static const FlipperAppPluginDescriptor plugin_remote_descriptor = {
    .appid = REMOTE_PLUGIN_APP_ID,
    .ep_api_version = REMOTE_PLUGIN_API_VERSION,
    .entry_point = &plugin_remote,
};

/* Plugin entry point - must return a pointer to const descriptor */
const FlipperAppPluginDescriptor* plugin_remote_ep(void) {
    return &plugin_remote_descriptor;
}
