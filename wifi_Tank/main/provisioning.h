#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// Start provisioning. Returns 0 if STA started (credentials found in NVS).
// Does not return if in SoftAP mode (blocks until reboot after config save).
// Outputs the wifi_event_group needed by app_main.
int ProvisioningStart(EventGroupHandle_t *wifi_event_group_out);

// Erase stored credentials from NVS (triggers SoftAP on next boot).
void ProvisioningClearCredentials(void);
