#include "hal_b_esp32.h"

#include "esp_timer.h"
#include "esp_rom_sys.h"

// Waiting for the echo line to rise. The HC-SR04 gives up on its own at around
// 38 ms; 30 ms covers its full range and still fails fast.
static const int64_t ECHO_START_TIMEOUT_US = 30000;

// Widest echo we accept, ~4.3 m round trip. Beyond that it is noise.
static const int64_t ECHO_MAX_WIDTH_US = 25000;

// Shortest echo that can be acoustic. The sensor's minimum range is ~2 cm,
// which is 116 us of round trip. Anything shorter is electrical.
static const uint32_t ECHO_MIN_WIDTH_US = 116;

HalBEsp32::HalBEsp32(gpio_num_t trig, gpio_num_t echo)
    : trig_(trig), echo_(echo) {}

void HalBEsp32::init()
{
    gpio_config_t out = {};
    out.pin_bit_mask = 1ULL << trig_;
    out.mode         = GPIO_MODE_OUTPUT;
    gpio_config(&out);

    gpio_config_t in = {};
    in.pin_bit_mask = 1ULL << echo_;
    in.mode         = GPIO_MODE_INPUT;
    // Unlike 34-39, GPIO 18 does have internal pulls. The divider's 2k leg is
    // what really holds this line low; the 45k pull-down costs nothing and
    // turns a loose wire into a clean NO-RISE instead of 60 Hz mains pickup
    // dressed up as distance readings.
    in.pull_up_en   = GPIO_PULLUP_DISABLE;
    in.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&in);

    gpio_set_level(trig_, 0);
}

uint32_t HalBEsp32::nowMs() const
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

uint16_t HalBEsp32::distanceCm()
{
    last_width_us_ = 0;

    // Guard against a line still high from the previous cycle. Without this,
    // the first wait below would exit instantly and we would end up timing the
    // tail of the old pulse instead of a new one.
    int64_t guard = esp_timer_get_time();
    while (gpio_get_level(echo_) == 1) {
        if (esp_timer_get_time() - guard > 5000) {
            last_result_ = EchoResult::NoFall;
            return 0;
        }
    }

    // The HC-SR04 starts a burst on a 10 us HIGH pulse on TRIG.
    gpio_set_level(trig_, 0);
    esp_rom_delay_us(4);
    gpio_set_level(trig_, 1);
    esp_rom_delay_us(10);
    gpio_set_level(trig_, 0);

    // Busy-wait polling rather than an interrupt plus a hardware capture. At
    // ~5 samples per second the CPU cost is irrelevant, and a loop with an
    // explicit timeout is far easier to reason about than an ISR.
    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(echo_) == 0) {
        if (esp_timer_get_time() - t0 > ECHO_START_TIMEOUT_US) {
            last_result_ = EchoResult::NoRise;
            return 0;
        }
    }

    int64_t rise = esp_timer_get_time();
    while (gpio_get_level(echo_) == 1) {
        if (esp_timer_get_time() - rise > ECHO_MAX_WIDTH_US) {
            last_result_ = EchoResult::NoFall;
            return 0;
        }
    }

    last_width_us_ = (uint32_t)(esp_timer_get_time() - rise);

    // Report a too-short pulse as its own case. Letting integer division turn
    // it into 0 would make it indistinguishable from "no echo at all", which
    // is exactly the ambiguity that hid the problem in the first run.
    if (last_width_us_ < ECHO_MIN_WIDTH_US) {
        last_result_ = EchoResult::ShortPulse;
        return 0;
    }

    // Sound travels ~343 m/s at 20 C, i.e. 0.0343 cm/us. The pulse spans the
    // round trip, so each centimetre of distance costs 2 / 0.0343 = 58.3 us.
    last_result_ = EchoResult::Ok;
    return (uint16_t)(last_width_us_ / 58);
}