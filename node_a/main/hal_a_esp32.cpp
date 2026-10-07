#include "hal_a_esp32.h"
#include "esp_timer.h"

static const bool LED_COMMON_ANODE = false;

HalAEsp32::HalAEsp32(gpio_num_t pir_out, gpio_num_t buzz, gpio_num_t r, gpio_num_t g, gpio_num_t b) : pir_out_(pir_out), buzz_(buzz), r_(r), g_(g), b_(b) {}

void HalAEsp32::init()
{
    // Configurate all the pins for the RGB LED.
    gpio_config_t io {};
    io.mode         = GPIO_MODE_OUTPUT;
    io.pin_bit_mask = (1ULL << r_) | (1ULL << g_) | (1ULL << b_);

    gpio_config(&io);

    // Buzzer: INPUT_OUTPUT, not plain OUTPUT. A pin set to GPIO_MODE_OUTPUT has
    // its input buffer disabled, so gpio_get_level() on it always returns 0.
    // alarmActive() reports what the pin is really doing, so the buffer stays on.
    io = {};
    io.mode         = GPIO_MODE_INPUT_OUTPUT;
    io.pin_bit_mask = 1ULL << buzz_;
    gpio_config(&io);

    // PIR output. Reset the struct first so the settings above do not carry over.
    io = {};
    io.mode         = GPIO_MODE_INPUT;
    io.pin_bit_mask = 1ULL << pir_out_;
    // GPIO 32 was chosen over 34-39 because it has this pull-down: a detached
    // signal wire then reads a clean 0 instead of picking up 60 Hz mains hum.
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&io);

    setBuzzer(false);
    setLed(Led::Off);

}

// Get the movement of the sensor
uint32_t HalAEsp32::nowMs() const
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

bool HalAEsp32::motionDetected()
{
    // The HC-SR501 drives its output HIGH while it holds a detection, for the
    // dwell time set by its delay pot. Seconds, not microseconds, which is why
    // polling is enough and a GPIO interrupt would buy nothing.
    return gpio_get_level(pir_out_) == 1;
}


bool HalAEsp32::alarmActive()
{
    // Read the pin back instead of a flag, so what the node reports is what the hardware is doing.
    return gpio_get_level(buzz_) == 1;
}

void HalAEsp32::setBuzzer(bool on)
{
    // Active buzzer: it carries its own oscillator, so a level is all it needs.
    gpio_set_level(buzz_, on ? 1 : 0);
}

// Set color of the RGB LED based on the provided led enum value
void HalAEsp32::setLed(Led c)
{
    //Yellow is red and green together
    bool r = (c == Led::Red)    || (c == Led::Yellow);
    bool g = (c == Led::Green)  || (c == Led::Yellow);

    gpio_set_level(r_, LED_COMMON_ANODE ? !r : r);
    gpio_set_level(g_, LED_COMMON_ANODE ? !g : g);
    // Blue is unused, but drive it explicitly so it cannot stay lit.
    gpio_set_level(b_, LED_COMMON_ANODE ? 1 : 0);    
}