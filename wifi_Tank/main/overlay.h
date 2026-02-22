/*! \file overlay.h
\brief Video overlay management for WebSocket-based overlay system
*******************************************************************************/

#ifndef OVERLAY_H_
#define OVERLAY_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "esp_http_server.h"

// Maximum overlay elements
#define OVERLAY_MAX_TEXT 10
#define OVERLAY_MAX_SHAPES 20
#define OVERLAY_MAX_TEXT_LENGTH 64
#define OVERLAY_MAX_COLOR_LENGTH 16

// Shape types
typedef enum {
    OVERLAY_SHAPE_LINE = 0,
    OVERLAY_SHAPE_RECT,
    OVERLAY_SHAPE_CIRCLE,
    OVERLAY_SHAPE_TRIANGLE
} overlay_shape_type_t;

// Text overlay element
typedef struct {
    char content[OVERLAY_MAX_TEXT_LENGTH];
    int16_t x;
    int16_t y;
    char color[OVERLAY_MAX_COLOR_LENGTH];
    uint8_t size;
} overlay_text_t;

// Shape overlay element
typedef struct {
    overlay_shape_type_t type;
    int16_t x1, y1;  // Start point or center (for circle)
    int16_t x2, y2;  // End point or width/height (for rect)
    int16_t x3, y3;  // Third vertex (for triangle)
    int16_t radius;  // For circle
    char color[OVERLAY_MAX_COLOR_LENGTH];
    uint8_t width;   // Line width
    bool fill;       // Fill shape (for rect/circle)
} overlay_shape_t;

// Motor state for track-bar feedback overlay
typedef struct {
    int8_t l;   // -100..100  positive = fwd, negative = rev, 0 = stop
    int8_t r;
} overlay_motors_t;

// Telemetry data collected every overlay update cycle
typedef struct {
    float    fps;           // Camera frames per second
    int8_t   rssi;          // WiFi RSSI in dBm (negative)
    uint8_t  wifi_channel;  // WiFi channel 1-13
    uint32_t tx_kbps;       // Application TX throughput kbps
    uint32_t rx_kbps;       // Application RX throughput kbps
    uint32_t int_heap_kb;   // Free internal heap KB (not PSRAM)
    float    mcu_temp_c;    // MCU temperature °C; -127.0 if unavailable
    uint32_t uptime_s;      // System uptime seconds
    uint16_t cam_aec;       // Camera auto-exposure value 0-1200 (lower = brighter scene)
    uint8_t  cam_gain;      // Camera AGC gain 0-30 (higher = more noise / low light)
} overlay_telemetry_t;

// Complete overlay data structure
typedef struct {
    uint8_t text_count;
    overlay_text_t texts[OVERLAY_MAX_TEXT];

    uint8_t shape_count;
    overlay_shape_t shapes[OVERLAY_MAX_SHAPES];

    bool               has_motors;
    overlay_motors_t   motors;

    bool               has_telemetry;
    overlay_telemetry_t telemetry;
} overlay_data_t;

/**
 * @brief Initialize overlay system with WebSocket support
 *
 * @param server HTTP server handle to attach WebSocket endpoint
 * @return 0 on success, -1 on failure
 */
int OverlayInit(httpd_handle_t server);

/**
 * @brief Send overlay update to all connected WebSocket clients
 *
 * @param overlay Overlay data to send
 * @return Number of clients updated, or -1 on error
 */
int OverlaySendUpdate(const overlay_data_t *overlay);

/**
 * @brief Create sample overlay data for testing
 *
 * @param overlay Pointer to overlay structure to populate
 */
void OverlayCreateSampleData(overlay_data_t *overlay);

/**
 * @brief Get number of connected WebSocket clients
 *
 * @return Number of active WebSocket connections
 */
int OverlayGetClientCount(void);

#ifdef __cplusplus
}
#endif

#endif /* OVERLAY_H_ */
