
#include "interface.h"
#include <flipper_application/flipper_application.h>

#include <furi_hal.h>

#include <lib/subghz/subghz_tx_rx_worker.h>
#include "helpers/radio_device_loader.h"
#include <stdint.h>

#define _TAG "PicoPassRemote"

const SubGhzDevice* device;
SubGhzTxRxWorker* subghz_txrx;

RemoteCallback rx_callback;
void* rx_callback_context;
uint8_t* rx_buffer;
size_t rx_buffer_len = 0;

// Header and footer to identify messages for us
const char* HEADER = "PP-R-NRM<";
const uint8_t HEADER_SIZE = 9;
const char* FOOTER = ">";
const uint8_t FOOTER_SIZE = 1;

// Don't use 433.920MHz as it's a harmonic frequency of the 13.56MHz signal used for the card
const uint32_t FREQUENCY = 433560000;

bool is_suppressing_charge = false;

void rx_event_callback(void* ctx) {
    FURI_LOG_D("PPNRM", "RX saw smth!");
    UNUSED(ctx);

    // Read up to 32 bytes at a time
    // This is just an arbitrary size, small enough to hopefully not cause
    //  OOM issues, but large enough to be able to recieve a full message.
    uint8_t buffer[32];
    int len = (int)subghz_tx_rx_worker_read(subghz_txrx, buffer, sizeof(buffer));
    if(len <= 0) {
        FURI_LOG_D("PPNRM", "RX len<0!");
        return;
    }
    if (rx_buffer == NULL) {
        rx_buffer = malloc(len);
        rx_buffer_len = 0;
    } else {
        rx_buffer = realloc(rx_buffer, rx_buffer_len + len);
    }
    memcpy(rx_buffer + rx_buffer_len, buffer, len);
    rx_buffer_len += len;
    
    FURI_LOG_D("PPNRM", "RX Here rx_buf_len=%d, len=%d!", rx_buffer_len, len);

    // Search for the header in the buffer
    for (size_t i = 0; i < rx_buffer_len - HEADER_SIZE; i++) {
        FURI_LOG_D("PPNRM", "RX Buffer %02X (%c) @ %d", rx_buffer[i], rx_buffer[i], i);
    }
    for (size_t i = 0; i < rx_buffer_len - HEADER_SIZE; i++) {
        if (memcmp(rx_buffer+i, HEADER, HEADER_SIZE) == 0) {
            FURI_LOG_D("PPNRM", "Found header @ %d", i);
            i += HEADER_SIZE;
            uint8_t dataLength = rx_buffer[i++];
            FURI_LOG_D("PPNRM", "dataLength=%d > rx_buffer_len=%d - i=%d - FOOTER_SIZE=%d", dataLength, rx_buffer_len, i, FOOTER_SIZE);

            if (dataLength > rx_buffer_len - i - FOOTER_SIZE) {
                // Not enough for a full message, wait for more data
                FURI_LOG_D("PPNRM", "Not enough bytes for footer");
                return;
            }
            // Found a header, now look for the footer
            if (memcmp(rx_buffer+i+dataLength, FOOTER, FOOTER_SIZE) == 0) {
                FURI_LOG_D("PPNRM", "Found footer");
                // Valid header and footer, send body to callback
                uint8_t* data = malloc(dataLength);
                memcpy(data, rx_buffer + i, dataLength);
                for (size_t j = 0; j < dataLength; j++) {
                    FURI_LOG_D("PPNRM", "RX Final output data %02X (%c) @ %d", data[j], data[j], j);
                }
                rx_callback(rx_callback_context, data, dataLength);
                free(data);

                int remainderIdx = i + dataLength + FOOTER_SIZE;
                int remainder = rx_buffer_len - remainderIdx;
                if (remainder > 0) {
                    // Copy anything after the footer to the beginning of the buffer
                    memmove(rx_buffer, rx_buffer + remainderIdx, remainder);
                    rx_buffer = realloc(rx_buffer, rx_buffer_len);
                    rx_buffer_len = remainder;
                } else {
                    // If the footer is the end of the buffer, just free it
                    free(rx_buffer);
                    rx_buffer = NULL;
                    rx_buffer_len = 0;
                }
                break;
            } else {
                FURI_LOG_D("PPNRM", "No footer found");
                // Invalid footer, discard everything up to the header
                memmove(rx_buffer, rx_buffer + i, rx_buffer_len - i);
                rx_buffer = realloc(rx_buffer, rx_buffer_len - i);
                rx_buffer_len = rx_buffer_len - i;
            }

            // Reset i to 0 to search for the next header
            i = 0;
        }
    }
}

void remote_free() {
    if(is_suppressing_charge) {
        furi_hal_power_suppress_charge_exit();
        is_suppressing_charge = false;
    }

    if (rx_buffer != NULL) {
        free(rx_buffer);
        rx_buffer = NULL;
        rx_buffer_len = 0;
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
    // Add header and footer
    uint8_t full_len = len+HEADER_SIZE+FOOTER_SIZE+1;
    uint8_t full_data[full_len];
    memcpy(full_data, HEADER, HEADER_SIZE);
    full_data[HEADER_SIZE] = len;
    memcpy(full_data+HEADER_SIZE+1, data, len);
    memcpy(full_data+HEADER_SIZE+1+len, FOOTER, FOOTER_SIZE);

    for (int i = 0; i < full_len; i++) {
        FURI_LOG_D("PPNRM", "TX Buffer %02X (%c) @ %d", full_data[i], full_data[i], i);
    }
 
    while(!subghz_tx_rx_worker_write(subghz_txrx, full_data, full_len)) {
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
