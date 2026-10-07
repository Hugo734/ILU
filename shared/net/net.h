#pragma once
#include <stdint.h>

#include "esp_err.h"
#include "protocol.h"

void net_init(void);
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
