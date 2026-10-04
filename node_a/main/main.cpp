#include "esp_log.h"
#include "protocol.h"
#include "net.h"

static const char *TAG = "node_a";

// node_b, read from its eFuse with `esptool.py read_mac`.
static const uint8_t PEER_MAC[6] = { 0x78, 0x42, 0x1c, 0x68, 0x44, 0x98 };

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Nodo a | protocolo v%d", PROTOCOL_VERSION);
    net_init();
    espnow_init(PEER_MAC, 'A');
}