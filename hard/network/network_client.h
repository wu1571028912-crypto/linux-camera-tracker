#ifndef NETWORK_CLIENT_H
#define NETWORK_CLIENT_H

#include <stdint.h>

#define NETWORK_OK 0
#define NETWORK_ERR_ARGUMENT (-1)
#define NETWORK_ERR_CONNECT (-2)
#define NETWORK_ERR_IO (-3)
#define NETWORK_ERR_TIMEOUT (-4)
#define NETWORK_ERR_PROTOCOL (-5)

typedef enum {
    NETWORK_TRACK_IDLE = 0,
    NETWORK_TRACKING = 1,
    NETWORK_TRACK_LOST = 2,
    NETWORK_TRACK_ERROR = 3
} NetworkTrackState;

typedef struct {
    int valid;
    int x;
    int y;
} NetworkTouch;

typedef struct {
    uint32_t frame_id;
    NetworkTrackState state;
    int x1;
    int y1;
    int x2;
    int y2;
    int center_x;
    int center_y;
    int confidence_milli;
} NetworkTrackResult;

/* One NetworkClient is used by one thread. The caller owns all image memory. */
typedef struct {
    int socket_fd;
    uint32_t next_frame_id;
    int timeout_ms;
    int initialized;
} NetworkClient;

#define NETWORK_CLIENT_INITIALIZER { -1, 1U, 0, 0 }

/* Initialize client with NETWORK_CLIENT_INITIALIZER before its first use. */
int network_client_init(NetworkClient *client, const char *server_ip,
                        uint16_t server_port, int timeout_ms);

/*
 * Sends one caller-owned RGB565 frame and waits for its result. Coordinates
 * use source-image pixels. A transport or protocol error may close client.
 */
int network_client_exchange(NetworkClient *client, const uint16_t *rgb565,
                            int width, int height, int stride,
                            uint64_t timestamp_ms, const NetworkTouch *touch,
                            NetworkTrackResult *result);

void network_client_close(NetworkClient *client);

#endif
