#pragma once

#include <stdint.h>

/** Start the GB 46750-2025 Wi-Fi beacon broadcaster. */
void remoteIdStart(void);

/** Return the latest GB 46750 field 015 operation state. */
uint8_t remoteIdGetOperationState(void);
