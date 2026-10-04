#include "esp_log.h"
#include "protocol.h"
#include "net.h"

static const char *TAG = "node_b";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Nodo b | protocolo v%d", PROTOCOL_VERSION);
    net_init();
}
