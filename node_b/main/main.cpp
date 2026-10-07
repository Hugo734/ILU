#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hal_b_esp32.h"
#include "net.h"
#include "protocol.h"

static const char *TAG = "node_b";

static const uint16_t CERCA_MIN = 4;
static const uint16_t CERCA_MAX = 15;

// node_a, read from its eFuse with esptool read_mac. Lives here and not in a
// shared header because each node's peer is the other one.
static const uint8_t PEER_MAC[6] = {0x78, 0x42, 0x1c, 0x68, 0x44, 0x98};

// The access state is repeated, not only sent on change: a single lost
// AccessClosed would otherwise leave node_a disarmed until the next flip.
// node_a's lease (3 s) must stay a few multiples of this.
static const uint32_t STATE_REPEAT_MS = 1000;

static const uint32_t BLINK_HALF_MS = 500;

static void send_access_state(bool open, uint16_t *seq, uint32_t now)
{
    Frame f{};
    f.version   = PROTOCOL_VERSION;
    f.type      = static_cast<uint8_t>(MsgType::Event);
    f.src       = 'B';
    f.event     = static_cast<uint8_t>(open ? EventId::AccessOpen : EventId::AccessClosed);
    f.seq       = (*seq)++;
    f.uptime_ms = now;

    esp_err_t err = espnow_send_frame(f);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TX %s seq %u: %s", open ? "AccessOpen" : "AccessClosed",
                 (unsigned)f.seq, esp_err_to_name(err));
    }
}

extern "C" void app_main(void)
{
    HalBEsp32 hal(GPIO_NUM_5, GPIO_NUM_18, /* access switch */ GPIO_NUM_4, /* RGB LED */ GPIO_NUM_25, GPIO_NUM_26, GPIO_NUM_27);
    hal.init();

    // Channel test before the radio comes up: if the wiring is wrong, you see it
    // in six seconds instead of after a screen of Wi-Fi logs.
    ESP_LOGW(TAG, "Channel test: RED / GREEN / YELLOW / BLUE, 1.5 s each");
    hal.setLed(Led::Red);    vTaskDelay(pdMS_TO_TICKS(1500));
    hal.setLed(Led::Green);  vTaskDelay(pdMS_TO_TICKS(1500));
    hal.setLed(Led::Yellow); vTaskDelay(pdMS_TO_TICKS(1500));
    hal.setLed(Led::Blue);   vTaskDelay(pdMS_TO_TICKS(1500));

    net_init();
    espnow_init(PEER_MAC);

    bool     alert       = false;  // motion reported while access was closed
    bool     prev_access = false;
    bool     prev_cerca  = false;
    bool     first       = true;   // forces the first state frame and log line
    uint32_t last_tx     = 0;
    uint16_t seq         = 0;
    bool     have_rx_seq = false;
    uint16_t last_rx_seq = 0;

    while (true) {
        uint32_t now    = hal.nowMs();
        uint16_t cm     = hal.distanceCm();
        bool     access = hal.accessSwitchOn();
        bool     cerca  = (cm >= CERCA_MIN && cm <= CERCA_MAX);

        Frame f;
        while (espnow_receive(&f)) {
            // Gaps are logged, not acted on: every message is safe to miss or
            // repeat. A gap back to 0 usually means node_a rebooted.
            if (have_rx_seq && f.seq != (uint16_t)(last_rx_seq + 1)) {
                ESP_LOGW(TAG, "RX seq jumped %u -> %u (lost frames or node_a rebooted)",
                         (unsigned)last_rx_seq, (unsigned)f.seq);
            }
            have_rx_seq = true;
            last_rx_seq = f.seq;

            if (f.type == static_cast<uint8_t>(MsgType::Event) &&
                f.event == static_cast<uint8_t>(EventId::MotionStarted)) {
                if (access) {
                    ESP_LOGI(TAG, "RX MotionStarted seq %u: access open, ignored", (unsigned)f.seq);
                } else {
                    ESP_LOGW(TAG, "RX MotionStarted seq %u: ALERT", (unsigned)f.seq);
                    alert = true;
                }
            }
        }

        // Opening access is how someone acknowledges the alert.
        if (access && alert) {
            ESP_LOGI(TAG, "alert cleared by access switch");
            alert = false;
        }

        if (first || access != prev_access || now - last_tx >= STATE_REPEAT_MS) {
            send_access_state(access, &seq, now);
            last_tx = now;
        }

        if (access) {
            hal.setLed(Led::Green);
        } else if (alert) {
            hal.setLed(((now / BLINK_HALF_MS) % 2) ? Led::Red : Led::Off);
        } else if (cerca) {
            hal.setLed(Led::Yellow);
        } else {
            hal.setLed(Led::Red);
        }

        // Log on change only, so the radio lines stay visible.
        if (first || access != prev_access || cerca != prev_cerca) {
            uint32_t rejected, queue_full;
            espnow_rx_counters(&rejected, &queue_full);
            ESP_LOGI(TAG, "%3u cm | access %s | %s | rx rejected %lu, queue full %lu",
                     cm, access ? "OPEN" : "CLOSED", cerca ? "CERCA" : "-",
                     (unsigned long)rejected, (unsigned long)queue_full);
        }
        prev_access = access;
        prev_cerca  = cerca;
        first       = false;

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
