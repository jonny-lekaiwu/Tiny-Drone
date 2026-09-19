#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Call once after NVS initialization, before starting any flight tasks.
 * State is immutable until reboot: writing activation never unlocks motors live. */
/* Disable automatic restart when activation does not interlock flight. */
esp_err_t deviceActivationInit(bool restartAfterActivation);
bool deviceActivationIsActive(void);
const char *deviceActivationProductId(void);
/* Empty until activated; otherwise the last eight characters of the UAS mark. */
const char *deviceActivationRegistrationId(void);
