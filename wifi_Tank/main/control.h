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
 * Return the current (post-ramp) motor values being applied.
 * Values are -100..100 (positive = forward, negative = reverse, 0 = stopped).
 * Safe to call from any task.
 */
void ControlGetMotorState(int *l, int *r);

/**
 * Set the ramp rate in %/s (1–500).  Persisted in NVS immediately.
 * The ramp task uses this value on the next tick.
 */
void ControlSetRampRate(int percent_per_sec);

/** Return the active ramp rate in %/s. */
int  ControlGetRampRate(void);

#ifdef __cplusplus
}
#endif
