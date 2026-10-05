#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hal_b_esp32.h"

static const char *TAG = "node_b";

static const uint16_t CERCA_MIN = 2;
static const uint16_t CERCA_MAX = 10;
static const uint32_t ACCESO_MS = 3000;

extern "C" void app_main(void)
{
    HalBEsp32 hal(GPIO_NUM_5, GPIO_NUM_18, GPIO_NUM_4, GPIO_NUM_13,
                  GPIO_NUM_25, GPIO_NUM_26, GPIO_NUM_27);
    hal.init();

    // Prueba de canales. Borra este bloque cuando el LED ya de los colores
    // correctos.
    ESP_LOGW(TAG, "prueba: ROJO / VERDE / AMARILLO, 1.5 s cada uno");
    hal.setLed(Led::Red);    vTaskDelay(pdMS_TO_TICKS(1500));
    hal.setLed(Led::Green);  vTaskDelay(pdMS_TO_TICKS(1500));
    hal.setLed(Led::Yellow); vTaskDelay(pdMS_TO_TICKS(1500));

    bool     acceso   = false;
    uint32_t t_acceso = 0;

    while (true) {
        uint32_t now = hal.nowMs();
        uint16_t cm  = hal.distanceCm();
        bool     btn = hal.buttonPressed();

        if (btn) { acceso = true; t_acceso = now; }
        if (acceso && now - t_acceso >= ACCESO_MS) acceso = false;

        bool cerca = (cm >= CERCA_MIN && cm <= CERCA_MAX);

        if (acceso) {
            hal.setLed(Led::Green);
            hal.setServoAngle(90);
        } else if (cerca) {
            hal.setLed(Led::Yellow);
            hal.setServoAngle(0);
        } else {
            hal.setLed(Led::Red);
            hal.setServoAngle(0);
        }

        ESP_LOGI(TAG, "%3u cm | btn %d | %s", cm, btn,
                 acceso ? "ACCESO" : (cerca ? "CERCA" : "CERRADO"));

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}