#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_http_server.h"

/**
 * Register the /ctrl WebSocket endpoint on an existing HTTP server.
 *
 * Receives {"key":"w/a/s/d","pressed":true/false} frames and drives
 * motors accordingly.  Motors are stopped automatically whenever the
 * client disconnects or sends a close frame.
 *
 * BTS7960B WASD mapping:
 *   W — both tracks forward
 *   S — both tracks reverse
 *   A — pivot left  (left REV, right FWD)
 *   D — pivot right (left FWD, right REV)
 *   release (pressed=false) — stop all
 *
 * @param server  Running httpd_handle_t (same instance as the overlay).
 * @return        0 on success, -1 on failure.
 */
int ControlInit(httpd_handle_t server);

/**
 * Return the last motor values applied by the control WebSocket.
 * Values are -100..100 (positive = forward, negative = reverse, 0 = stopped).
 * Safe to call from any task.
 */
void ControlGetMotorState(int *l, int *r);

#ifdef __cplusplus
}
#endif
