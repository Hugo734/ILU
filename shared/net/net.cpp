#include "net.h"
#include <string.h>
#include <stdint.h>
#include <atomic>
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
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
// ESP-NOW transport
// ---------------------------------------------------------------------------

// Room for a burst of motion edges while the main loop is busy; 8 x 10 bytes.
static const UBaseType_t RX_QUEUE_LEN = 8;

static uint8_t       s_peer_mac[6];
static QueueHandle_t s_rx_queue = nullptr;

// Written in the Wi-Fi task, read in the app task: atomic so a read never
// sees a half-updated value.
static std::atomic<uint32_t> s_rx_rejected{0};
static std::atomic<uint32_t> s_rx_queue_full{0};

// Runs in the Wi-Fi task. It must not block or do heavy work (no per-frame
// logging), so it only filters, copies the frame onto the queue and returns.
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    // ESP-NOW delivers frames from any device on the channel, not only the
    // registered peer. Until encryption is added, the MAC is the only filter.
    if (memcmp(info->src_addr, s_peer_mac, 6) != 0 || len < 0) {
        s_rx_rejected++;
        return;
    }

    Frame f;
    if (!protocol_parse(data, (size_t)len, &f)) {
        s_rx_rejected++;
        return;
    }

    // Timeout 0: waiting for space here would stall the whole Wi-Fi stack.
    if (xQueueSend(s_rx_queue, &f, 0) != pdTRUE) {
        s_rx_queue_full++;
    }
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

void espnow_init(const uint8_t peer_mac[6])
{
    memcpy(s_peer_mac, peer_mac, 6);

    // Created before the receive callback is registered: a frame arriving
    // during start-up would otherwise be pushed to a queue that does not exist.
    s_rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(Frame));
    if (s_rx_queue == nullptr) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

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

    // PMK/LMK encryption comes in a later step.
    peer.encrypt = false;

    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    ESP_LOGI(TAG, "ESP-NOW ready | peer %02x:%02x:%02x:%02x:%02x:%02x",
             peer_mac[0], peer_mac[1], peer_mac[2],
             peer_mac[3], peer_mac[4], peer_mac[5]);
}

esp_err_t espnow_send_frame(const Frame &f)
{
    return esp_now_send(s_peer_mac, reinterpret_cast<const uint8_t *>(&f), sizeof f);
}

bool espnow_receive(Frame *out)
{
    // nullptr check: called before espnow_init(), there is simply nothing yet.
    return s_rx_queue != nullptr && xQueueReceive(s_rx_queue, out, 0) == pdTRUE;
}

void espnow_rx_counters(uint32_t *rejected, uint32_t *queue_full)
{
    *rejected   = s_rx_rejected.load();
    *queue_full = s_rx_queue_full.load();
}
