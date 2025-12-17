/*! \file system.h
\brief System initialization and TCP server management
*******************************************************************************/

#ifndef SYSTEM_H_
#define SYSTEM_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Initialize the system
 *
 * Creates a system task that manages the TCP server and other system functions.
 * This should be called after WiFi is connected.
 *
 * @param tcp_port TCP server port number (use 0 to disable TCP server)
 */
void SystemInit(uint16_t tcp_port);


/**
 * @brief Get the maximum TCP payload size
 *
 * Returns the size of a normal full TCP packet payload (MSS - Maximum Segment Size).
 * Useful for optimizing data transmission.
 *
 * @return Maximum TCP payload size in bytes
 */
size_t SystemTcpGetPayloadSize(void);

/**
 * @brief Send data to all connected TCP clients
 *
 * @param data Pointer to data buffer to send
 * @param len Length of data to send
 * @return Number of bytes sent, or -1 on error
 */
int SystemTcpSendToClients(const uint8_t *data, size_t len);

/**
 * @brief Get the number of connected TCP clients
 *
 * @return Number of active client connections
 */
int SystemTcpGetClientCount(void);

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_H_ */
