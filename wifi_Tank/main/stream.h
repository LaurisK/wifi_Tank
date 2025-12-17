/*! \file stream.h
\brief Video streaming module for ESP32-CAM
*******************************************************************************/

#ifndef STREAM_H_
#define STREAM_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Initialize the video streaming system
 *
 * Initializes the camera (OV3660 on AI-Thinker ESP32-CAM) and creates
 * an HTTP MJPEG streaming server.
 *
 * @param stream_port HTTP port for video stream (use 0 to disable)
 * @return 0 on success, -1 on failure
 */
int StreamInit(uint16_t stream_port);

/**
 * @brief Start the video stream
 *
 * Begins capturing and streaming video frames.
 *
 * @return 0 on success, -1 on failure
 */
int StreamStart(void);

/**
 * @brief Stop the video stream
 *
 * Stops capturing and streaming video frames.
 */
void StreamStop(void);

/**
 * @brief Check if stream is active
 *
 * @return true if streaming, false otherwise
 */
bool StreamIsActive(void);

/**
 * @brief Get the number of connected stream clients
 *
 * @return Number of active clients watching the stream
 */
int StreamGetClientCount(void);

/**
 * @brief Get current frame rate
 *
 * @return Current FPS (frames per second)
 */
float StreamGetFps(void);

#ifdef __cplusplus
}
#endif

#endif /* STREAM_H_ */
