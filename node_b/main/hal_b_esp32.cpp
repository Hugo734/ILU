#include "hal_b_esp32.h"

#include "driver/ledc.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

// Cambia a true si tu LED es de anodo comun.
static const bool LED_ANODO_COMUN = false;

// SG90: pulso de 0.5 a 2.5 ms repetido cada 20 ms. Con un timer de 50 Hz
// a 13 bits el periodo son 8192 cuentas, asi que 0.5 ms = 205 y 2.5 ms = 1024.
static const uint32_t SERVO_MIN = 205;
static const uint32_t SERVO_MAX = 1024;

HalBEsp32::HalBEsp32(gpio_num_t trig, gpio_num_t echo, gpio_num_t btn,
                     gpio_num_t servo, gpio_num_t r, gpio_num_t g, gpio_num_t b)
    : trig_(trig), echo_(echo), btn_(btn), servo_(servo), r_(r), g_(g), b_(b) {}

void HalBEsp32::init()
{
    gpio_config_t io = {};
    io.mode         = GPIO_MODE_OUTPUT;
    io.pin_bit_mask = (1ULL << trig_) | (1ULL << r_) | (1ULL << g_) | (1ULL << b_);
    gpio_config(&io);

    io = {};
    io.mode         = GPIO_MODE_INPUT;
    io.pin_bit_mask = 1ULL << echo_;
    // Si el cable se zafa, el pin lee 0 limpio en vez de captar la red.
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&io);

    io = {};
    io.mode         = GPIO_MODE_INPUT;
    io.pin_bit_mask = 1ULL << btn_;
    // La otra pata va a GND, por eso presionado lee 0 y suelto lee 1.
    io.pull_up_en   = GPIO_PULLUP_ENABLE;
    gpio_config(&io);

    // El servo necesita un pulso preciso cada 20 ms. LEDC lo genera en
    // hardware; hacerlo a mano ocuparia la CPU permanentemente.
    ledc_timer_config_t t = {};
    t.speed_mode      = LEDC_LOW_SPEED_MODE;
    t.timer_num       = LEDC_TIMER_0;
    t.duty_resolution = LEDC_TIMER_13_BIT;
    t.freq_hz         = 50;
    t.clk_cfg         = LEDC_AUTO_CLK;
    ledc_timer_config(&t);

    ledc_channel_config_t c = {};
    c.gpio_num   = servo_;
    c.speed_mode = LEDC_LOW_SPEED_MODE;
    c.channel    = LEDC_CHANNEL_0;
    c.timer_sel  = LEDC_TIMER_0;
    ledc_channel_config(&c);

    setLed(Led::Off);
    setServoAngle(0);
}

uint32_t HalBEsp32::nowMs() const
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

bool HalBEsp32::buttonPressed()
{
    return gpio_get_level(btn_) == 0;
}

void HalBEsp32::setLed(Led c)
{
    bool r = (c == Led::Red)   || (c == Led::Yellow);
    bool g = (c == Led::Green) || (c == Led::Yellow);

    gpio_set_level(r_, LED_ANODO_COMUN ? !r : r);
    gpio_set_level(g_, LED_ANODO_COMUN ? !g : g);
    gpio_set_level(b_, LED_ANODO_COMUN ? 1  : 0);
}

void HalBEsp32::setServoAngle(uint8_t deg)
{
    if (deg > 180) deg = 180;
    uint32_t duty = SERVO_MIN + (SERVO_MAX - SERVO_MIN) * deg / 180;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

uint16_t HalBEsp32::distanceCm()
{
    gpio_set_level(trig_, 1);
    esp_rom_delay_us(10);
    gpio_set_level(trig_, 0);

    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(echo_) == 0) {
        if (esp_timer_get_time() - t0 > 30000) return 0;   // no contesto
    }

    int64_t rise = esp_timer_get_time();
    while (gpio_get_level(echo_) == 1) {
        if (esp_timer_get_time() - rise > 25000) return 0; // fuera de rango
    }

    // 58 us por cm: el sonido va a 0.0343 cm/us y el pulso cubre ida y vuelta.
    return (uint16_t)((esp_timer_get_time() - rise) / 58);
}