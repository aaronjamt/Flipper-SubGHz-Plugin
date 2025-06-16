#include <flipper_application/flipper_application.h>

#include "interface.h"

bool remote_load_layer(const char *name, void **storage);

void remote_set_rx_cb(RemoteCallback callback, void* context);
void remote_write(uint8_t* data, size_t len);

bool remote_init();
bool remote_start(uint32_t frequency);
void remote_stop();
void remote_free();

/* Actual implementation of app<>plugin interface */
static const PluginRemote plugin_remote = {
    .init = &remote_init,
    .load_layer = &remote_load_layer,
    .start = &remote_start,

    .set_rx_cb = &remote_set_rx_cb,
    .write = &remote_write,

    .stop = &remote_stop,
    .free = &remote_free,
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