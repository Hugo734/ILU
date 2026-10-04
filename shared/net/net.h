#pragma once

#include <stdint.h>

void net_init(void);
void espnow_init(const uint8_t peer_mac[6], char self_id);
