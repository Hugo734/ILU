#include "net.h"
#include <string.h>
#include <stdint.h>
#include "esp_now.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_err.h"
#include "nvs_flash.h"


static const char *TAG = "net";


void net_init() {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize the TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());

    // Create default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize Wi-Fi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    // configure the start every time from the code instead of using flash memory
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    ESP_LOGI(TAG, "Wi-Fi MAC address: %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ---------------------------------------------------------------------------
// ESP-NOW spike
// ---------------------------------------------------------------------------

// NOT the protocol.md frame yet. This only proves the link, the peer
// registration and the callbacks. The real frame replaces this struct later.
//
// packed: without it the compiler inserts 3 padding bytes after `src` to align
// `counter`. Sender and receiver are separate binaries, so any disagreement on
// field offsets silently corrupts every field after the first.
typedef struct __attribute__((packed)) {
    uint8_t  src;        // 'A' or 'B'
    uint32_t counter;
    uint32_t uptime_ms;
} SpikeMsg;

static_assert(sizeof(SpikeMsg) == 9, "SpikeMsg must stay 9 bytes on the wire");

static uint8_t s_peer_mac[6];
static char    s_self_id;

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    // Drop anything that is not exactly the expected size. Without this guard a
    // short packet would make the memcpy below read past the end of the buffer.
    if (len != (int)sizeof(SpikeMsg)) {
        ESP_LOGW(TAG, "RX dropped: %d bytes, expected %d", len, (int)sizeof(SpikeMsg));
        return;
    }

    // Copy into a local instead of casting the pointer: the driver's buffer has
    // no alignment guarantee, and reading a uint32_t from an unaligned address
    // is undefined behaviour.
    SpikeMsg msg;
    memcpy(&msg, data, sizeof(msg));

    ESP_LOGI(TAG, "RX %02x:%02x:%02x:%02x:%02x:%02x | node %c | counter %lu | uptime %lu ms",
             info->src_addr[0], info->src_addr[1], info->src_addr[2],
             info->src_addr[3], info->src_addr[4], info->src_addr[5],
             msg.src,
             (unsigned long)msg.counter,
             (unsigned long)msg.uptime_ms);
}

static void on_sent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    // tx_info carries source/destination addresses; with a single peer it adds
    // nothing yet, so it stays unused.
    (void)tx_info;

    // IMPORTANT: this is the 802.11 link-layer ACK. It only means the peer's
    // radio acknowledged the frame at MAC level. It does NOT mean the remote
    // application received, parsed or executed anything. Requirement #6 needs a
    // separate application-level ACK on top of this.
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "TX got no link-layer ACK");
    }
}

static void tx_task(void *arg)
{
    (void)arg;
    uint32_t counter = 0;

    while (true) {
        SpikeMsg msg;
        msg.src       = (uint8_t)s_self_id;
        msg.counter   = counter++;
        msg.uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);

        esp_err_t err = esp_now_send(s_peer_mac, (const uint8_t *)&msg, sizeof(msg));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_now_send: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void espnow_init(const uint8_t peer_mac[6], char self_id)
{
    memcpy(s_peer_mac, peer_mac, 6);
    s_self_id = self_id;

    // Must run after esp_wifi_start(): ESP-NOW rides on an already-running radio.
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_sent));

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, peer_mac, 6);

    // channel 0 means "whatever channel the station interface is already on".
    // Hardcoding a number here would force both boards onto that exact channel.
    peer.channel = 0;

    // Must name an interface that is actually up. This is why the node has to
    // run in STA mode and not softAP.
    peer.ifidx = WIFI_IF_STA;

    // PMK/LMK encryption comes in a later step, together with the real frame.
    peer.encrypt = false;

    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    xTaskCreate(tx_task, "espnow_tx", 3072, nullptr, 5, nullptr);

    ESP_LOGI(TAG, "ESP-NOW ready | self %c | peer %02x:%02x:%02x:%02x:%02x:%02x",
             s_self_id,
             peer_mac[0], peer_mac[1], peer_mac[2],
             peer_mac[3], peer_mac[4], peer_mac[5]);
}