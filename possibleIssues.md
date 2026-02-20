# Possible Issues

## High Severity

### Hardcoded WiFi Credentials (`provisioning.c:30–33`)
WiFi SSIDs and passwords are hardcoded in plaintext in `s_defaults[]`. They are visible in the compiled binary and in version control history. Anyone with repo or binary access gets the credentials. Consider storing defaults outside of source (e.g. a local config header excluded from git, or entered only via the SoftAP provisioning flow).

### `ws_async_send` Called from Wrong Task (`overlay.c:319`)
Despite its name, `ws_async_send` is invoked directly from `overlay_demo_task` rather than being queued via `httpd_queue_work()`. Calling `httpd_ws_send_frame_async` from outside the httpd task is not supported and can cause crashes or data corruption. The function should be submitted to the httpd task using `httpd_queue_work(overlay_state.server, ws_async_send, ws_pkt)`.

## Medium Severity

### `client_count` Unprotected and Can Go Negative (`overlay.c:194–203`)
`overlay_state.client_count` is decremented in `ws_async_send` when disconnected clients are found, but is only reset at the start of `OverlaySendUpdate`. If clients disconnect between calls, the counter can go negative. There is also no mutex protecting this shared state.

### FD Scanning for WebSocket Clients (`overlay.c:283–309`, `370–374`)
Scanning fd values from `3` to `CONFIG_LWIP_MAX_SOCKETS` to discover active WebSocket clients is fragile — it may miss clients on some configurations and wastes cycles on every update. The standard approach is to register clients in a WebSocket open callback and remove them on close.

### `StreamGetFps` Logic Is Incorrect (`stream.c:324–337`)
The calculation `1000.0f / elapsed_ms_since_last_frame` returns the inverse of time since the last frame, not actual frames per second. It would read `10.0` if the last frame was 100ms ago regardless of how many frames were sent. A sliding window or frame-count-over-time approach is needed for accurate FPS.

### Throughput Counters Unsynchronized (`main.c:39–45`, `101–130`)
`app_throughput_add_rx/tx` and `throughput_monitor_task` access shared `app_throughput` fields from different FreeRTOS tasks without a mutex or atomic operations. On a dual-core ESP32, `uint32_t` accesses are not guaranteed to be atomic across cores, risking torn reads/writes.

## Low Severity

### Partial TCP Send Not Handled (`system.c:288–292`)
When `send()` returns fewer bytes than requested, only a warning is logged and the client stays connected with a truncated message. This leaves the receiving end in an undefined protocol state. The remaining bytes should either be retried or the client should be disconnected.

### `nvs_read_network` Passes Sizes by Value (`provisioning.c:69–82`)
`nvs_get_str` expects a pointer to `size_t` which it updates with the actual string length read. The function receives `ssid_sz` and `pass_sz` as local copies, so the size update is silently discarded. Currently benign, but is an API misuse that could cause bugs if the function is extended.
