/**
 * @file interface.h
 * @brief Remote plugin interface.
 *
 * Common interface between the Remote plugin and host application
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <furi.h>
#include <furi_hal.h>
#include "hex.h"

#define REMOTE_PLUGIN_APP_ID "plugin_remote"
#define REMOTE_PLUGIN_API_VERSION 1

typedef void (*RemoteCallback)(void* context, uint8_t* data, size_t length);

typedef struct {
    const char* name;
    bool (*init)();
    void (*free)();
    void (*write)(uint8_t* data, size_t len);
    void (*set_rx_cb)(RemoteCallback process_line, void* context);
} PluginRemote;