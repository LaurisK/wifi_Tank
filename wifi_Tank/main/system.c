/*! \file system.c
\brief System initialization and TCP server management implementation
*******************************************************************************/

#include "system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/tcp.h"
#include <string.h>
#include <errno.h>

static const char *TAG = "SYSTEM";

// TCP server configuration
#define MAX_CLIENTS 4
#define TCP_KEEPALIVE_IDLE 5
#define TCP_KEEPALIVE_INTERVAL 5
#define TCP_KEEPALIVE_COUNT 3
#define SYSTEM_TASK_STACK_SIZE 4096
#define SYSTEM_TASK_PRIORITY 5

// Client connection structure
typedef struct {
    int socket;
    bool connected;
    struct sockaddr_in addr;
} tcp_client_t;

// System state
static struct {
    int server_socket;
    uint16_t server_port;
    tcp_client_t clients[MAX_CLIENTS];
    SemaphoreHandle_t client_mutex;
    TaskHandle_t system_task;
    bool running;
} system_state = {
    .server_socket = -1,
    .server_port = 0,
    .client_mutex = NULL,
    .system_task = NULL,
    .running = false
};

/**
 * @brief Create and start the TCP server (internal function)
 */
static int tcp_server_create(uint16_t port) {
    ESP_LOGI(TAG, "Creating TCP server on port %d", port);

    if (system_state.server_socket >= 0) {
        ESP_LOGW(TAG, "TCP server already running");
        return -1;
    }

    // Create socket
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return -1;
    }

    // Set socket to non-blocking
    int flags = fcntl(listen_sock, F_GETFL, 0);
    fcntl(listen_sock, F_SETFL, flags | O_NONBLOCK);

    // Enable address reuse
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind socket
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    int err = bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket bind failed: errno %d", errno);
        close(listen_sock);
        return -1;
    }

    // Listen for connections
    err = listen(listen_sock, MAX_CLIENTS);
    if (err != 0) {
        ESP_LOGE(TAG, "Socket listen failed: errno %d", errno);
        close(listen_sock);
        return -1;
    }

    system_state.server_socket = listen_sock;
    system_state.server_port = port;

    ESP_LOGI(TAG, "TCP server listening on port %d", port);
    return 0;
}

/**
 * @brief Accept new client connection
 */
static void accept_new_client(void) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int client_sock = accept(system_state.server_socket,
                            (struct sockaddr *)&client_addr,
                            &addr_len);

    if (client_sock < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGE(TAG, "accept() failed: errno %d", errno);
        }
        return;
    }

    // Set socket to non-blocking mode
    int flags = fcntl(client_sock, F_GETFL, 0);
    fcntl(client_sock, F_SETFL, flags | O_NONBLOCK);

    // Enable TCP keepalive
    int keepalive = 1;
    setsockopt(client_sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(int));

    int keepidle = TCP_KEEPALIVE_IDLE;
    setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(int));

    int keepintvl = TCP_KEEPALIVE_INTERVAL;
    setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(int));

    int keepcnt = TCP_KEEPALIVE_COUNT;
    setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(int));

    // Find free slot for client
    xSemaphoreTake(system_state.client_mutex, portMAX_DELAY);

    bool added = false;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!system_state.clients[i].connected) {
            system_state.clients[i].socket = client_sock;
            system_state.clients[i].connected = true;
            system_state.clients[i].addr = client_addr;

            ESP_LOGI(TAG, "New client connected from %s:%d (slot %d)",
                    inet_ntoa(client_addr.sin_addr),
                    ntohs(client_addr.sin_port), i);

            added = true;
            break;
        }
    }

    xSemaphoreGive(system_state.client_mutex);

    if (!added) {
        ESP_LOGW(TAG, "Maximum clients reached, rejecting connection");
        close(client_sock);
    }
}

/**
 * @brief Check and cleanup disconnected clients
 */
static void cleanup_disconnected_clients(void) {
    xSemaphoreTake(system_state.client_mutex, portMAX_DELAY);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (system_state.clients[i].connected) {
            // Try to peek if socket is still alive
            char buf;
            int ret = recv(system_state.clients[i].socket, &buf, 1, MSG_PEEK | MSG_DONTWAIT);

            if (ret == 0 || (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                // Connection closed or error
                ESP_LOGI(TAG, "Client %d disconnected", i);
                close(system_state.clients[i].socket);
                system_state.clients[i].connected = false;
                system_state.clients[i].socket = -1;
            }
        }
    }

    xSemaphoreGive(system_state.client_mutex);
}

/**
 * @brief System task - manages TCP server
 */
static void system_task(void *pvParameters) {
    ESP_LOGI(TAG, "System task started");

    while (system_state.running) {
        // Accept new clients if server is running
        if (system_state.server_socket >= 0) {
            accept_new_client();
        }

        // Check for disconnected clients
        cleanup_disconnected_clients();

        // Small delay to prevent busy-waiting
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "System task stopped");
    vTaskDelete(NULL);
}

void SystemInit(uint16_t tcp_port) {
    ESP_LOGI(TAG, "Initializing system");

    // Create mutex for client list protection
    system_state.client_mutex = xSemaphoreCreateMutex();
    if (system_state.client_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create client mutex");
        return;
    }

    // Initialize client slots
    for (int i = 0; i < MAX_CLIENTS; i++) {
        system_state.clients[i].socket = -1;
        system_state.clients[i].connected = false;
    }

    // Create system task
    system_state.running = true;
    BaseType_t ret = xTaskCreate(
        system_task,
        "system_task",
        SYSTEM_TASK_STACK_SIZE,
        NULL,
        SYSTEM_TASK_PRIORITY,
        &system_state.system_task
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create system task");
        system_state.running = false;
        return;
    }

    // Create TCP server if port is specified
    if (tcp_port > 0) {
        if (tcp_server_create(tcp_port) == 0) {
            ESP_LOGI(TAG, "TCP server created on port %d", tcp_port);
            ESP_LOGI(TAG, "TCP payload size: %zu bytes", SystemTcpGetPayloadSize());
        } else {
            ESP_LOGE(TAG, "Failed to create TCP server");
        }
    }

    ESP_LOGI(TAG, "System initialized successfully");
}


size_t SystemTcpGetPayloadSize(void) {
    // TCP MSS (Maximum Segment Size) for ESP32
    // Typically: MTU(1500) - IP_header(20) - TCP_header(20) = 1460 bytes
    // However, we'll use a slightly smaller value for safety
    return 1400;
}

int SystemTcpSendToClients(const uint8_t *data, size_t len) {
    if (data == NULL || len == 0) {
        return -1;
    }

    int total_sent = 0;

    xSemaphoreTake(system_state.client_mutex, portMAX_DELAY);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (system_state.clients[i].connected) {
            int sent = send(system_state.clients[i].socket, data, len, 0);

            if (sent < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    ESP_LOGW(TAG, "Send to client %d failed: errno %d", i, errno);
                    // Mark for cleanup
                    system_state.clients[i].connected = false;
                }
            } else {
                total_sent += sent;

                if (sent < len) {
                    ESP_LOGW(TAG, "Partial send to client %d: %d/%d bytes", i, sent, len);
                }
            }
        }
    }

    xSemaphoreGive(system_state.client_mutex);

    return total_sent;
}

int SystemTcpGetClientCount(void) {
    int count = 0;

    xSemaphoreTake(system_state.client_mutex, portMAX_DELAY);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (system_state.clients[i].connected) {
            count++;
        }
    }

    xSemaphoreGive(system_state.client_mutex);

    return count;
}
