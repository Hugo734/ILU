#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hal_a_esp32.h"
#include "command.h"
#include "protocol.h"
#include "net.h"

static const char *TAG = "node_a";

// node_b, read from its eFuse with esptool read_mac. Lives here and not in a
// shared header because each node's peer is the other one.
static const uint8_t PEER_MAC[6] = {0xf4, 0x65, 0x0b, 0xc0, 0xe0, 0xa4};

// node_b repeats its access state every second. Access counts as open only
// while a recent AccessOpen backs it, so a dead node_b, a lost AccessClosed or
// a broken link all end with the alarm armed, never disarmed.
static const uint32_t ACCESS_LEASE_MS = 3000;

// node_b has no other way to know this node is alive: a motion alert only
// goes out when something happens. node_b treats 3 s of silence as a lost link.
// While the alarm is latched, MotionStarted takes the heartbeat's place.
static const uint32_t HEARTBEAT_MS = 1000;

// node_b blinks on this node's clock (it learns it from every frame), so with
// the same half-period both LEDs blink together.
static const uint32_t BLINK_HALF_MS = 500;

// Any valid frame from node_b counts as a sign of life; it sends at least
// one per second.
static const uint32_t PEER_TIMEOUT_MS = 3000;

// Remotely controllable variables. The platform addresses them by name; the
// index is the enum value. Values reset to the defaults on reboot, and the
// state report always carries the values actually in effect.
enum Var { BUZZER_ENABLED, WARMUP_S, PUBLISH_MS, ACCESS, BUZZER_ON, VAR_COUNT };
static const VarSpec VARS[VAR_COUNT] = {
    // 0 = silent alarm: the LED still turns red and node_b is still alerted.
    {"buzzer_enabled", 0, 1},
    // The HC-SR501 fires on its own while its pyroelectric element settles.
    {"warmup_s", 0, 300},
    // The platform marks a node offline after 3 s without a report.
    {"publish_ms", 250, 2000},
    // The platform's Open/Close button goes to both nodes on ilu/all/cmd.
    // Here it acts on the access lease (see apply_access), so node_b's own
    // frames still decide within 3 s and a platform "open" alone cannot keep
    // the room disarmed.
    {"access", 0, 1},
    // Manual buzzer from the platform, independent of the alarm.
    {"buzzer_on", 0, 1},
};
static int32_t cfg[VAR_COUNT] = {1, 60, 1000, 0, 0};

// Access lease state. File scope because both the command handler and the
// ESP-NOW receive path write it; both run in the main task.
static bool     open_heard   = false;
static uint32_t last_open_ms = 0;

// An "access" command from the platform is applied exactly like a frame from
// node_b: open starts a lease, close ends it at once. Closing immediately is
// the safe direction; opening only lasts while node_b keeps confirming it.
static void apply_access(bool open, uint32_t now)
{
    open_heard   = open;
    last_open_ms = now;
}

static Frame make_frame(MsgType type, EventId event, uint16_t *seq, uint32_t now)
{
    Frame f{};
    f.version   = PROTOCOL_VERSION;
    f.type      = static_cast<uint8_t>(type);
    f.src       = 'A';
    f.event     = static_cast<uint8_t>(event);
    f.seq       = (*seq)++;
    f.uptime_ms = now;
    return f;
}

// Applies every queued command and acknowledges its execution. Runs at the
// top of the loop, so a new value already drives this iteration's outputs
// and the state report that follows.
static void handle_commands(uint32_t now)
{
    Command c;
    while (command_receive(&c)) {
        ApplyResult r = command_apply(VARS, cfg, VAR_COUNT, c);
        if (r.stage == AckStage::Applied && r.index == ACCESS) apply_access(cfg[ACCESS] != 0, now);
        bool known = r.index >= 0;
        char ack[160];
        if (ack_format(ack, sizeof ack, 'a', c.id, c.var, r.stage, r.reason, known, known ? cfg[r.index] : 0)) {
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
    ESP_LOGI(TAG, "Nodo a | protocolo v%d", PROTOCOL_VERSION);

    HalAEsp32 hal(GPIO_NUM_32, GPIO_NUM_14,
                  GPIO_NUM_25, GPIO_NUM_26, GPIO_NUM_27);
    hal.init();

    // Channel test before the radio comes up: if the wiring is wrong, you see it
    // in four seconds instead of after a screen of Wi-Fi logs.
    ESP_LOGW(TAG, "Channel test: RED / GREEN / YELLOW / BLUE, 1.5 s each");
    hal.setLed(Led::Red);    vTaskDelay(pdMS_TO_TICKS(1500));
    hal.setLed(Led::Green);  vTaskDelay(pdMS_TO_TICKS(1500));
    hal.setLed(Led::Yellow); vTaskDelay(pdMS_TO_TICKS(1500));
    hal.setLed(Led::Blue);   vTaskDelay(pdMS_TO_TICKS(1500));
    hal.setLed(Led::Off);

    ESP_LOGW(TAG, "Buzzer test: 200 ms");
    hal.setBuzzer(true);  vTaskDelay(pdMS_TO_TICKS(200));
    hal.setBuzzer(false);
    net_init('a');
    espnow_init(PEER_MAC);

    ESP_LOGW(TAG, "PIR warm-up: %ld s. Readings before that are not trustworthy.", (long)cfg[WARMUP_S]);

    uint32_t t0   = hal.nowMs();
    bool     prev = false;
    bool     prev_alarm = false;
    uint16_t seq  = 0;
    bool     prev_access  = false;
    // Latched alarm: set by motion while armed, cleared only when access
    // opens (switch or platform). The buzzer keeps sounding until then.
    bool     latched      = false;
    bool     near_b       = false;   // node_b reports someone in its near window
    bool     peer_heard   = false;
    uint32_t last_peer_ms = 0;
    bool     prev_peer    = false;
    uint32_t last_hb      = 0;
    char     last_body[256] = "";
    uint32_t last_pub     = 0;

    while (true) {
        uint32_t now    = hal.nowMs();
        handle_commands(now);

        bool     warm   = (now - t0) >= (uint32_t)cfg[WARMUP_S] * 1000;
        bool     motion = hal.motionDetected();

        Frame f;
        while (espnow_receive(&f)) {
            peer_heard   = true;
            last_peer_ms = now;
            if (f.type != static_cast<uint8_t>(MsgType::Event)) continue;
            if (f.event == static_cast<uint8_t>(EventId::AccessOpen)) {
                apply_access(true, now);
            } else if (f.event == static_cast<uint8_t>(EventId::AccessClosed)) {
                apply_access(false, now);
            } else if (f.event == static_cast<uint8_t>(EventId::NearOn)) {
                near_b = true;
            } else if (f.event == static_cast<uint8_t>(EventId::NearOff)) {
                near_b = false;
            }
        }
        bool access = open_heard && (now - last_open_ms) < ACCESS_LEASE_MS;
        bool peer   = peer_heard && (now - last_peer_ms) < PEER_TIMEOUT_MS;

        if (access != prev_access) {
            ESP_LOGW(TAG, "access %s%s", access ? "OPEN: alarm disarmed" : "CLOSED: alarm armed",
                     (!access && open_heard) ? " (lease expired, node_b silent)" : "");
            prev_access = access;
        }
        if (peer != prev_peer) {
            ESP_LOGW(TAG, "ESP-NOW link to node_b %s", peer ? "up" : "LOST: staying armed");
            prev_peer = peer;
        }

        // Warm-up false triggers and authorised movement never latch, so they
        // never reach node_b either.
        if (access) latched = false;
        else if (warm && motion) latched = true;
        bool alarm = latched;
        if (alarm != prev_alarm) {
            ESP_LOGW(TAG, "%s", alarm ? "ALARM latched: buzzer on until access opens" : "alarm cleared: access open");
        }

        // The manual buzzer from the platform sounds on its own; the alarm
        // sounds unless buzzer_enabled made it a silent alarm.
        hal.setBuzzer((alarm && cfg[BUZZER_ENABLED] != 0) || cfg[BUZZER_ON] != 0);

        // Same priority order as node_b, so both LEDs always show the same
        // colour. node_b's near window is mirrored here; yellow is only this
        // node's PIR warm-up, which node_b has no equivalent of.
        bool        blink_on = ((now / BLINK_HALF_MS) % 2) == 0;
        const char *led;
        if (alarm) {
            hal.setLed(blink_on ? Led::Red : Led::Off);
            led = "red_blink";
        } else if (!peer) {
            hal.setLed(blink_on ? Led::Blue : Led::Off);
            led = "blue_blink";
        } else if (access) {
            hal.setLed(Led::Blue);
            led = "blue";
        } else if (!warm) {
            hal.setLed(Led::Yellow);
            led = "yellow";
        } else if (near_b) {
            hal.setLed(Led::Green);
            led = "green";
        } else {
            hal.setLed(Led::Red);
            led = "red";
        }

        // MotionStarted at once on the latch's rising edge, then every second
        // in place of the heartbeat while the alarm stays latched: a lost
        // frame cannot leave node_b unaware. Shares the sequence counter with
        // the heartbeat, so a gap node_b sees means lost frames.
        if ((alarm && !prev_alarm) || now - last_hb >= HEARTBEAT_MS) {
            Frame out = alarm ? make_frame(MsgType::Event, EventId::MotionStarted, &seq, now)
                              : make_frame(MsgType::Heartbeat, static_cast<EventId>(0), &seq, now);
            esp_err_t err = espnow_send_frame(out);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "TX %s seq %u: %s", alarm ? "MotionStarted" : "heartbeat",
                         (unsigned)out.seq, esp_err_to_name(err));
            }
            last_hb = now;
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

        // buzzer is read back from the pin, not taken from `alarm`: the platform
        // must show what the hardware is doing, not what the code intended.
        // The settings travel with the state, so after a command the platform
        // shows the value the node is using, not the one it sent.
        char body[256];
        snprintf(body, sizeof body,
                 "\"warm\":%s,\"motion\":%s,\"alarm\":%s,\"buzzer\":%d,\"access\":%s,\"near\":%s,\"peer\":%s,\"led\":\"%s\","
                 "\"buzzer_on\":%ld,\"buzzer_enabled\":%ld,\"warmup_s\":%ld,\"publish_ms\":%ld",
                 warm ? "true" : "false", motion ? "true" : "false", alarm ? "true" : "false",
                 (int)hal.alarmActive(), access ? "true" : "false", (near_b && peer) ? "true" : "false",
                 peer ? "true" : "false", led,
                 (long)cfg[BUZZER_ON], (long)cfg[BUZZER_ENABLED], (long)cfg[WARMUP_S], (long)cfg[PUBLISH_MS]);
        if (strcmp(body, last_body) != 0 || now - last_pub >= (uint32_t)cfg[PUBLISH_MS]) {
            char json[320];
            snprintf(json, sizeof json, "{\"node\":\"a\",\"up\":%lu,%s}", (unsigned long)now, body);
            mqtt_publish_state(json);
            strcpy(last_body, body);
            last_pub = now;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
