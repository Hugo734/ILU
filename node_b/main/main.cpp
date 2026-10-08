#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hal_b_esp32.h"
#include "command.h"
#include "net.h"
#include "protocol.h"

static const char *TAG = "node_b";

// Lower edge of the "near" window. Below 4 cm the HC-SR04 is unreliable; the
// upper edge is remotely controllable (near_cm).
static const uint16_t CERCA_MIN = 4;

// node_a, read from its eFuse with esptool read_mac. Lives here and not in a
// shared header because each node's peer is the other one.
static const uint8_t PEER_MAC[6] = {0x78, 0x42, 0x1c, 0x68, 0x44, 0x98};

// node_a sends a heartbeat every second. Three missed ones mean the room is
// no longer watched, and the door says so.
static const uint32_t PEER_TIMEOUT_MS = 3000;

static const uint32_t BLINK_HALF_MS = 500;

// Remotely controllable variables. The platform addresses them by name; the
// index is the enum value. Values reset to the defaults on reboot, and the
// state report always carries the values actually in effect.
enum Var { NEAR_CM, REPEAT_MS, PUBLISH_MS, VAR_COUNT };
static const VarSpec VARS[VAR_COUNT] = {
    // Upper edge of the "near" window. Must stay above CERCA_MIN.
    {"near_cm", 5, 100},
    // The access state is repeated, not only sent on change: a single lost
    // AccessClosed would otherwise leave node_a disarmed until the next flip.
    // node_a's lease is 3 s, so at most 1 s keeps three chances inside it.
    {"repeat_ms", 200, 1000},
    // The platform marks a node offline after 3 s without a report.
    {"publish_ms", 250, 2000},
};
static int32_t cfg[VAR_COUNT] = {15, 1000, 1000};

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

// Applies every queued command and acknowledges its execution. Runs at the
// top of the loop, so a new value already drives this iteration's outputs
// and the state report that follows.
static void handle_commands()
{
    Command c;
    while (command_receive(&c)) {
        ApplyResult r = command_apply(VARS, cfg, VAR_COUNT, c);
        bool known = r.index >= 0;
        char ack[160];
        if (ack_format(ack, sizeof ack, 'b', c.id, c.var, r.stage, r.reason, known, known ? cfg[r.index] : 0)) {
            mqtt_publish_ack(ack);
        }
        if (r.stage == AckStage::Applied) {
            ESP_LOGW(TAG, "cmd %s: %s = %ld applied", c.id, c.var, (long)cfg[r.index]);
        } else {
            ESP_LOGW(TAG, "cmd %s: %s = %ld rejected (%s)", c.id, c.var, (long)c.value, r.reason);
        }
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

    net_init('b');
    espnow_init(PEER_MAC);

    bool     alert       = false;  // motion reported while access was closed
    bool     prev_access = false;
    bool     prev_cerca  = false;
    bool     first       = true;   // forces the first state frame and log line
    uint32_t last_tx     = 0;
    uint16_t seq         = 0;
    bool     have_rx_seq = false;
    uint16_t last_rx_seq = 0;
    uint32_t last_peer_ms = 0;
    bool     prev_peer   = false;
    char     last_body[256] = "";
    uint32_t last_pub    = 0;

    while (true) {
        handle_commands();

        uint32_t now    = hal.nowMs();
        uint16_t cm     = hal.distanceCm();
        bool     access = hal.accessSwitchOn();
        bool     cerca  = (cm >= CERCA_MIN && cm <= (uint32_t)cfg[NEAR_CM]);

        Frame f;
        while (espnow_receive(&f)) {
            // Gaps are logged, not acted on: every message is safe to miss or
            // repeat. A gap back to 0 usually means node_a rebooted.
            if (have_rx_seq && f.seq != (uint16_t)(last_rx_seq + 1)) {
                ESP_LOGW(TAG, "RX seq jumped %u -> %u (lost frames or node_a rebooted)",
                         (unsigned)last_rx_seq, (unsigned)f.seq);
            }
            have_rx_seq  = true;
            last_rx_seq  = f.seq;
            last_peer_ms = now;

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

        // have_rx_seq doubles as "node_a has been heard at least once".
        bool peer = have_rx_seq && (now - last_peer_ms) < PEER_TIMEOUT_MS;
        if (peer != prev_peer) {
            ESP_LOGW(TAG, "ESP-NOW link to node_a %s", peer ? "up" : "LOST: room not watched (blue)");
            prev_peer = peer;
        }

        // Opening access is how someone acknowledges the alert.
        if (access && alert) {
            ESP_LOGI(TAG, "alert cleared by access switch");
            alert = false;
        }

        if (first || access != prev_access || now - last_tx >= (uint32_t)cfg[REPEAT_MS]) {
            send_access_state(access, &seq, now);
            last_tx = now;
        }

        // "red_blink" rather than the instantaneous on/off phase: the dashboard
        // animates the blink itself instead of receiving ten flips a second.
        // Blue sits below the alert: an intrusion already reported stays
        // visible even if node_a goes silent afterwards.
        const char *led;
        if (access) {
            hal.setLed(Led::Green);
            led = "green";
        } else if (alert) {
            hal.setLed(((now / BLINK_HALF_MS) % 2) ? Led::Red : Led::Off);
            led = "red_blink";
        } else if (!peer) {
            hal.setLed(Led::Blue);
            led = "blue";
        } else if (cerca) {
            hal.setLed(Led::Yellow);
            led = "yellow";
        } else {
            hal.setLed(Led::Red);
            led = "red";
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

        // Distance stays out of the change comparison: it jitters by a
        // centimetre, which would publish ten times a second. It still goes
        // out with every periodic message. The settings travel with the
        // state, so after a command the platform shows the value the node is
        // using, not the one it sent.
        char body[256];
        snprintf(body, sizeof body,
                 "\"near\":%s,\"access\":%s,\"alert\":%s,\"peer\":%s,\"led\":\"%s\","
                 "\"near_cm\":%ld,\"repeat_ms\":%ld,\"publish_ms\":%ld",
                 cerca ? "true" : "false", access ? "true" : "false",
                 alert ? "true" : "false", peer ? "true" : "false", led,
                 (long)cfg[NEAR_CM], (long)cfg[REPEAT_MS], (long)cfg[PUBLISH_MS]);
        if (strcmp(body, last_body) != 0 || now - last_pub >= (uint32_t)cfg[PUBLISH_MS]) {
            char json[320];
            snprintf(json, sizeof json, "{\"node\":\"b\",\"up\":%lu,\"cm\":%u,%s}",
                     (unsigned long)now, cm, body);
            mqtt_publish_state(json);
            strcpy(last_body, body);
            last_pub = now;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
