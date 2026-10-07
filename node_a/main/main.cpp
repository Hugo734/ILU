#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hal_a_esp32.h"
#include "protocol.h"
#include "net.h"

static const char *TAG = "node_a";

// The HC-SR501 fires on its own while its pyroelectric element settles.
// Drop this to 10000 while working at the bench, raise it before the demo.
static const uint32_t WARMUP_MS = 60000;

// node_b, read from its eFuse with esptool read_mac. Lives here and not in a
// shared header because each node's peer is the other one.
static const uint8_t PEER_MAC[6] = {0xf4, 0x65, 0x0b, 0xc0, 0xe0, 0xa4};

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Nodo a | protocolo v%d", PROTOCOL_VERSION);

    HalAEsp32 hal(GPIO_NUM_32, GPIO_NUM_14,
                  GPIO_NUM_25, GPIO_NUM_26, GPIO_NUM_27);
    hal.init();

    // Channel test before the radio comes up: if the wiring is wrong, you see it
    // in four seconds instead of after a screen of Wi-Fi logs.
    ESP_LOGW(TAG, "Channel test: RED / GREEN / YELLOW, 1.5 s each");
    hal.setLed(Led::Red);    vTaskDelay(pdMS_TO_TICKS(1500));
    hal.setLed(Led::Green);  vTaskDelay(pdMS_TO_TICKS(1500));
    hal.setLed(Led::Yellow); vTaskDelay(pdMS_TO_TICKS(1500));
    hal.setLed(Led::Off);

    ESP_LOGW(TAG, "Buzzer test: 200 ms");
    hal.setBuzzer(true);  vTaskDelay(pdMS_TO_TICKS(200));
    hal.setBuzzer(false);
    net_init();
    espnow_init(PEER_MAC);

    ESP_LOGW(TAG, "PIR warm-up: %lu s. Readings before that are not trustworthy.",
             (unsigned long)(WARMUP_MS / 1000));

    uint32_t t0   = hal.nowMs();
    bool     prev = false;
    bool     prev_alarm = false;
    uint16_t seq  = 0;

    while (true) {
        uint32_t now    = hal.nowMs();
        bool     warm   = (now - t0) >= WARMUP_MS;
        bool     motion = hal.motionDetected();

        // node_b gets the same signal node_a acts on, so warm-up false
        // triggers never reach it.
        bool alarm = warm && motion;

        if (alarm) {
            hal.setBuzzer(true);
            hal.setLed(Led::Red);
        } else {
            hal.setBuzzer(false);
            // Yellow while warming up, green once the sensor can be trusted.
            hal.setLed(warm ? Led::Green : Led::Yellow);
        }

        // Rising edge only: sending every iteration would flood the link with
        // ten frames a second for as long as someone stays in front of the PIR.
        if (alarm && !prev_alarm) {
            Frame f{};
            f.version   = PROTOCOL_VERSION;
            f.type      = static_cast<uint8_t>(MsgType::Event);
            f.src       = 'A';
            f.event     = static_cast<uint8_t>(EventId::MotionStarted);
            f.seq       = seq++;
            f.uptime_ms = now;

            esp_err_t err = espnow_send_frame(f);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "TX MotionStarted seq %u: ESP_OK", (unsigned)f.seq);
            } else {
                ESP_LOGE(TAG, "TX MotionStarted seq %u: %s", (unsigned)f.seq, esp_err_to_name(err));
            }
        }
        prev_alarm = alarm;

        // Log on change only, so the console stays readable. The buzzer figure is
        // read back from the pin, not the value just written - that difference is
        // what requirement #6 is about, and it is worth showing in the log.
        if (motion != prev) {
            ESP_LOGI(TAG, "%s | buzzer reported %d | warm %d | t=%lu ms",
                     motion ? ">>> MOTION" : "clear",
                     (int)hal.alarmActive(), (int)warm, (unsigned long)now);
            prev = motion;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}