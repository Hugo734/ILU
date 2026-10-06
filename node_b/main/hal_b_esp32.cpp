#include "hal_b_esp32.h"
//#include "driver/ledc.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"


static const bool LED_COMMON_ANODE = false;
HalBEsp32::HalBEsp32(gpio_num_t trig, gpio_num_t echo, gpio_num_t btn, gpio_num_t r, gpio_num_t g, gpio_num_t b)
    : trig_(trig), echo_(echo), btn_(btn), r_(r), g_(g), b_(b) {}

void HalBEsp32::init()
{
    // Configure the GPIO pins for the ultrasonic sensor, button, and RGB LED.
    gpio_config_t io = {};
    io.mode         = GPIO_MODE_OUTPUT;
    io.pin_bit_mask = (1ULL << trig_) | (1ULL << r_) | (1ULL << g_) | (1ULL << b_);
    gpio_config(&io);

    // Reset the struct between block
    // -----------Configure the GPIO pin for the echo signal from the ultrasonic sensor.----------------------------
    io = {};
    io.mode         = GPIO_MODE_INPUT;
    io.pin_bit_mask = 1ULL << echo_;
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&io);

    // Reset the struct between block
    // -----------Configure the GPIO pin for the button.----------------------------
    io = {};
    io.mode         = GPIO_MODE_INPUT;
    io.pin_bit_mask = 1ULL << btn_;
    // A loose wire then read a clean HIGH, so we need a pull-up to read LOW when pressed.
    io.pull_up_en   = GPIO_PULLUP_ENABLE;
    gpio_config(&io);

    setLed(Led::Off);

}

// Get the current time in milliseconds since the program started.
uint32_t HalBEsp32::nowMs() const
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Check if the button is pressed. Returns true if the button is pressed (LOW), false otherwise.
bool HalBEsp32::buttonPressed()
{
    return gpio_get_level(btn_) == 0;
}

// Set the color of the RGB LED based on the provided Led enum value.
void HalBEsp32::setLed(Led c)
{
    // Yellow is red and green lit together; the eye mixes them.
    bool r = (c == Led::Red)   || (c == Led::Yellow);
    bool g = (c == Led::Green) || (c == Led::Yellow);

    gpio_set_level(r_, LED_COMMON_ANODE ? !r : r);
    gpio_set_level(g_, LED_COMMON_ANODE ? !g : g);
    // Blue is unused, but drive it explicitly so it cannot stay lit.
    gpio_set_level(b_, LED_COMMON_ANODE ? 1 : 0);
}

uint16_t HalBEsp32::distanceCm()
{
    gpio_set_level(trig_, 1);
    esp_rom_delay_us(10);
    gpio_set_level(trig_, 0);

    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(echo_) == 0) {
        if (esp_timer_get_time() - t0 > 30000) return 0;   // sensor did not answer
    }

    int64_t rise = esp_timer_get_time();
    while (gpio_get_level(echo_) == 1) {
        if (esp_timer_get_time() - rise > 25000) return 0; // beyond ~430 cm
    }

    // Sound travels 0.0343 cm/us and the pulse covers the distance twice,
    // so one centimetre of range costs 2 / 0.0343 = 58.3 us. Calibrated at 20 C.
    return (uint16_t)((esp_timer_get_time() - rise) / 58);
}