/**
 * @file interface.h
 * @brief Remote plugin interface.
 *
 * Common interface between the Remote plugin and host application
 */
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

#include "layer.h"

#define REMOTE_PLUGIN_APP_ID "subghz_remote"
#define REMOTE_PLUGIN_API_VERSION 1

typedef void (*RemoteCallback)(void* context, uint8_t* data, size_t length);

typedef struct{
    bool (*init)();
    bool (*set_freq)(uint32_t frequency);
    bool (*load_layer)(const char *name, void **storage);

    void (*set_rx_cb)(RemoteCallback process_line, void* context);
    void (*write)(uint8_t* data, size_t len);

    void (*free)();
} PluginRemote;