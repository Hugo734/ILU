#pragma once
#include <stdint.h>

#include "esp_err.h"
#include "command.h"
#include "protocol.h"

// node_id 'a' or 'b' selects the MQTT topics ilu/<id>/state, /cmd, /ack and
// /status. Joins the access point in secrets.h and starts MQTT once an IP is
// assigned; both keep reconnecting on their own.
void net_init(char node_id);

// Fire-and-forget, QoS 0. Skipped while the broker is unreachable: the node
// must keep working with the platform gone (requirement #3).
void mqtt_publish_state(const char *json);

// Commands arrive on ilu/<id>/cmd and on ilu/all/cmd. The MQTT task parses
// each one, acknowledges reception at once ("received", or "rejected" if it
// cannot be parsed) and queues it. The main loop drains the queue here,
// applies the command and acknowledges execution with mqtt_publish_ack().
// Non-blocking.
bool command_receive(Command *out);

// QoS 1 on ilu/<id>/ack. Skipped while the broker is unreachable; the
// platform then shows the command as timed out, which is the truth.
void mqtt_publish_ack(const char *json);

void espnow_init(const uint8_t peer_mac[6]);

// Returns the esp_now_send() result instead of aborting: one busy-radio
// failure must not reboot the node, so the caller decides what it means.
esp_err_t espnow_send_frame(const Frame &f);

// Non-blocking. Only frames from the peer that passed protocol_parse() ever
// reach this queue, so the caller can trust what it gets.
bool espnow_receive(Frame *out);

// rejected: wrong sender MAC or failed protocol_parse().
// queue_full: valid frames lost because the main loop was not draining fast enough.
void espnow_rx_counters(uint32_t *rejected, uint32_t *queue_full);
