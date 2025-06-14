#include <flipper_application/flipper_application.h>

#include "interface.h"

#define TAG "SubGHzRemotePlugin"

bool load_layer(const char *name, void **storage);

void remote_set_rx_cb(RemoteCallback callback, void* context);
void remote_write(uint8_t* data, size_t len);

bool remote_init();
void remote_free();

/* Actual implementation of app<>plugin interface */
static const PluginRemote plugin_remote = {
    .init = &remote_init,
    .free = &remote_free,
    .write = &remote_write,
    .set_rx_cb = &remote_set_rx_cb,
    .load_layer = &load_layer,
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