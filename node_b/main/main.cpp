#include "esp_log.h"
#include "protocol.h"
#include "net.h"

static const char *TAG = "node_b";

// node_a, read from its eFuse with `esptool.py read_mac`.
static const uint8_t PEER_MAC[6] = { 0xf4, 0x65, 0x0b, 0xc0, 0xe0, 0xa4 };

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Nodo b | protocolo v%d", PROTOCOL_VERSION);
    net_init();
    espnow_init(PEER_MAC, 'B');
}