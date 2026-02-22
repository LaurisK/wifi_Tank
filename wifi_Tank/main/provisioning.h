#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// Start provisioning. Returns 0 if STA started (credentials found in NVS).
// Does not return if in SoftAP mode (blocks until reboot after config save).
// Outputs the wifi_event_group needed by app_main.
int ProvisioningStart(EventGroupHandle_t *wifi_event_group_out);

// Erase stored credentials from NVS (triggers SoftAP on next boot).
void ProvisioningClearCredentials(void);

// ── Runtime network management ────────────────────────────────────────────────

// Number of networks stored in NVS.
uint8_t ProvisioningGetNetCount(void);

// Read SSID and password for network at index idx. Returns ESP_OK on success.
esp_err_t ProvisioningReadNetwork(int idx, char *ssid, size_t ssid_sz,
                                  char *pass, size_t pass_sz);

// Append a new network (SSID + password) to NVS.
esp_err_t ProvisioningAddNetwork(const char *ssid, const char *pass);

// Update the password for an existing network (by index).
esp_err_t ProvisioningUpdateNetwork(int idx, const char *pass);

// Delete network at index idx, shifting later entries down.
esp_err_t ProvisioningDeleteNetwork(int idx);

// Index of the network currently connected to (-1 before first connection).
int ProvisioningGetCurrentNetIdx(void);
