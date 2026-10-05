#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "protocol.h"
#include "hal_b_esp32.h"

static const char *TAG = "node_b";

static const gpio_num_t PIN_TRIG = GPIO_NUM_5;   // I/O pin: can drive output
static const gpio_num_t PIN_ECHO = GPIO_NUM_18;  // fed by the 1k/2k divider

static const char *echoName(EchoResult r)
{
    switch (r) {
        case EchoResult::Ok:         return "OK";
        case EchoResult::ShortPulse: return "PULSO-CORTO";
        case EchoResult::NoRise:     return "NO-SUBE";
        case EchoResult::NoFall:     return "NO-BAJA";
    }
    return "?";
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Nodo b | protocolo v%d", PROTOCOL_VERSION);

    HalBEsp32 hal(PIN_TRIG, PIN_ECHO);
    hal.init();

    while (true) {
        uint16_t cm = hal.distanceCm();

        // One line per sample, always, whatever happened. A measurement that
        // fails silently teaches nothing.
        ESP_LOGI(TAG, "%-11s | %5lu us | %3u cm",
                 echoName(hal.lastResult()),
                 (unsigned long)hal.lastWidthUs(),
                 cm);

        // The HC-SR04 datasheet asks for at least 60 ms between triggers so the
        // previous burst has died out. 200 ms leaves a wide margin.
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}