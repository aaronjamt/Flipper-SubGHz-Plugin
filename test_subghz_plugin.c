#include <furi.h>



#include <flipper_application/plugins/plugin_manager.h>
#include <loader/firmware_api/firmware_api.h>

// Include the headers for the remote plugin and its layers
#include "plugin_remote/interface.h"
#include "plugin_remote/layers/header_footer.h"
// #include "plugin_remote/layers/checksum.h"
#include "plugin_remote/layers/slip.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define TAG "TestSubGHzApp"

void callback(void* context, uint8_t* data, size_t length) {
    UNUSED(context);

    // Example callback function
    printf("----------\n\n\e[1;7mCallback called with %u bytes of data: ", length);
    for (size_t i = 0; i < length; i++) {
        printf("%c", data[i]);
    }
    printf("\e[0m\n");
}

int32_t test_subghz_plugin(void* p) {
    UNUSED(p);

    // Load plugin
    PluginManager* plugin_remote_manager = plugin_manager_alloc(
        REMOTE_PLUGIN_APP_ID, REMOTE_PLUGIN_API_VERSION, firmware_api_interface);
    PluginRemote* plugin_remote = NULL;
    if(plugin_manager_load_single(
           plugin_remote_manager, APP_ASSETS_PATH("plugins/_plugin_remote.fal")) !=
       PluginManagerErrorNone) {
        FURI_LOG_E(TAG, "Failed to load Remote plugin");
    } else if(plugin_manager_get_count(plugin_remote_manager)) {
        plugin_remote =
            (PluginRemote*)plugin_manager_get_ep(plugin_remote_manager, 0);
    }
    
    // Initialize plugin
    plugin_remote->init();

    // Load layer 1: checksum
    if (!plugin_remote->load_layer("layer_checksum.fal", NULL)) {
        FURI_LOG_E(TAG, "Failed to load checksum layer");
        return -1;
    }

    // Load layer 2: header/footer
    HeaderFooterStorage *header_footer_storage;
    if (!plugin_remote->load_layer("layer_header_footer.fal", (void**)&header_footer_storage)) {
        FURI_LOG_E(TAG, "Failed to load header/footer layer");
        return -1;
    }
    header_footer_storage->configure(header_footer_storage, "PP-R-HF<", 8, ">", 1);

    // Load layer 3: modified SLIP
    SLIPStorage *slip_storage;
    if (!plugin_remote->load_layer("layer_slip.fal", (void**)&slip_storage)) {
        FURI_LOG_E(TAG, "Failed to load SLIP layer");
        return -1;
    }
    slip_storage->configure(slip_storage, 0x00, 0xFF, 0x7F, 0x01, 0xFE, 0x7E);



    // Set RX callback
    plugin_remote->set_rx_cb(callback, NULL);


    // Send data!
    uint8_t data[] = "Hello, world!";
    plugin_remote->write(data, sizeof(data));


    // Cleanup
    plugin_remote->free();
    return 0;
}
