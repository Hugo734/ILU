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
#include "mqtt_client.h"

#include "secrets.h"


static const char *TAG = "net";

static esp_mqtt_client_handle_t s_mqtt = nullptr;
static char                     s_state_topic[16];
// Set in the MQTT event task, read by the app task.
static std::atomic<bool>        s_mqtt_up{false};
// Touched only from the default event loop task, so no lock is needed.
static bool                     s_mqtt_started = false;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        auto *d = static_cast<wifi_event_sta_disconnected_t *>(data);
        ESP_LOGW(TAG, "Wi-Fi lost (reason %d), retrying", d->reason);
        // Retrying makes the radio scan for the AP, which can pull it off the
        // ESP-NOW channel. Kept simple until the router-off test shows a cost.
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *e = static_cast<ip_event_got_ip_t *>(data);
        uint8_t ch; wifi_second_chan_t second;
        esp_wifi_get_channel(&ch, &second);
        ESP_LOGI(TAG, "Wi-Fi up | ip " IPSTR " | channel %u", IP2STR(&e->ip_info.ip), ch);
        // Started here and not in net_init: before an IP exists every attempt
        // fails. After the first start, esp-mqtt handles reconnection itself.
        if (!s_mqtt_started) {
            esp_err_t err = esp_mqtt_client_start(s_mqtt);
            if (err == ESP_OK) s_mqtt_started = true;
            else ESP_LOGE(TAG, "esp_mqtt_client_start: %s", esp_err_to_name(err));
        }
    }
}

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    switch (static_cast<esp_mqtt_event_id_t>(id)) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_up = true;
        ESP_LOGI(TAG, "MQTT connected to %s", MQTT_BROKER_URI);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_up = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    default:
        break;
    }
}

void net_init(char node_id) {
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

    // The STA netif is what obtains an IP over DHCP; ESP-NOW alone never needed it.
    esp_netif_create_default_wifi_sta();

    // Initialize Wi-Fi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    // configure the start every time from the code instead of using flash memory
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t wc = {};
    strncpy((char *)wc.sta.ssid,     WIFI_SSID,     sizeof wc.sta.ssid);
    strncpy((char *)wc.sta.password, WIFI_PASSWORD, sizeof wc.sta.password);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    snprintf(s_state_topic, sizeof s_state_topic, "ilu/%c/state", node_id);
    esp_mqtt_client_config_t mc = {};
    mc.broker.address.uri = MQTT_BROKER_URI;
    s_mqtt = esp_mqtt_client_init(&mc);
    if (s_mqtt == nullptr) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_mqtt, MQTT_EVENT_ANY, on_mqtt_event, nullptr));

    // Registered before esp_wifi_start() so STA_START is not missed.
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, nullptr));

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

void mqtt_publish_state(const char *json)
{
    if (!s_mqtt_up) return;
    // Returns -1 only on a local failure (e.g. outbox full); a lost QoS 0
    // message is otherwise silent by design, the next state supersedes it.
    if (esp_mqtt_client_publish(s_mqtt, s_state_topic, json, 0, 0, 0) < 0) {
        ESP_LOGW(TAG, "MQTT publish failed");
    }
}
