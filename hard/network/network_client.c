#include "network_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define NETWORK_MAX_IMAGE_SIZE (2U * 1024U * 1024U)
#define NETWORK_MAGIC 0x5654524bU
#define NETWORK_VERSION 1U
#define NETWORK_MESSAGE_FRAME 1U
#define NETWORK_MESSAGE_RESULT 2U
#define NETWORK_PIXEL_RGB565_LE 1U
#define NETWORK_TOUCH_VALID 1U
#define NETWORK_HEADER_SIZE 12U
#define NETWORK_REQUEST_META_SIZE 36U
#define NETWORK_RESPONSE_BODY_SIZE 40U

static void clear_result(NetworkTrackResult *result)
{
    if (result == NULL)
        return;
    memset(result, 0, sizeof(*result));
    result->state = NETWORK_TRACK_ERROR;
}

int network_client_init(NetworkClient *client, const char *server_ip,
                        uint16_t server_port, int timeout_ms)
{
    struct sockaddr_in address;
    struct timeval timeout;
    int fd;

    if (client == NULL)
        return NETWORK_ERR_ARGUMENT;
    if (client->initialized && client->socket_fd >= 0)
        return NETWORK_ERR_ARGUMENT;
    client->socket_fd = -1;
    client->next_frame_id = 1;
    client->timeout_ms = timeout_ms;
    client->initialized = 1;
    if (server_ip == NULL || server_port == 0 || timeout_ms <= 0)
        return NETWORK_ERR_ARGUMENT;

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &address.sin_addr) != 1)
        return NETWORK_ERR_ARGUMENT;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return NETWORK_ERR_CONNECT;

    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                   &timeout, sizeof(timeout)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout)) < 0 ||
        connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return NETWORK_ERR_CONNECT;
    }

    client->socket_fd = fd;
    return NETWORK_OK;
}

void network_client_close(NetworkClient *client)
{
    if (client == NULL)
        return;
    if (client->initialized && client->socket_fd >= 0)
        close(client->socket_fd);
    client->socket_fd = -1;
    if (client->next_frame_id == 0)
        client->next_frame_id = 1;
    client->timeout_ms = 0;
    client->initialized = 1;
}

static void put_u16(uint8_t **cursor, uint16_t value)
{
    uint16_t encoded = htons(value);
    memcpy(*cursor, &encoded, sizeof(encoded));
    *cursor += sizeof(encoded);
}

static void put_u32(uint8_t **cursor, uint32_t value)
{
    uint32_t encoded = htonl(value);
    memcpy(*cursor, &encoded, sizeof(encoded));
    *cursor += sizeof(encoded);
}

static void put_u64(uint8_t **cursor, uint64_t value)
{
    put_u32(cursor, (uint32_t)(value >> 32));
    put_u32(cursor, (uint32_t)value);
}

static uint16_t get_u16(const uint8_t **cursor)
{
    uint16_t encoded;
    memcpy(&encoded, *cursor, sizeof(encoded));
    *cursor += sizeof(encoded);
    return ntohs(encoded);
}

static uint32_t get_u32(const uint8_t **cursor)
{
    uint32_t encoded;
    memcpy(&encoded, *cursor, sizeof(encoded));
    *cursor += sizeof(encoded);
    return ntohl(encoded);
}

static int socket_error_code(void)
{
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return NETWORK_ERR_TIMEOUT;
    return NETWORK_ERR_IO;
}

static int send_all(int fd, const void *data, size_t size)
{
    const uint8_t *cursor = data;

    while (size > 0) {
        ssize_t count = send(fd, cursor, size, MSG_NOSIGNAL);
        if (count > 0) {
            cursor += (size_t)count;
            size -= (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count == 0)
            return NETWORK_ERR_IO;
        return socket_error_code();
    }
    return NETWORK_OK;
}

static int recv_all(int fd, void *data, size_t size)
{
    uint8_t *cursor = data;

    while (size > 0) {
        ssize_t count = recv(fd, cursor, size, 0);
        if (count > 0) {
            cursor += (size_t)count;
            size -= (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count == 0)
            return NETWORK_ERR_IO;
        return socket_error_code();
    }
    return NETWORK_OK;
}

static int valid_box(int state, int x1, int y1, int x2, int y2,
                     int center_x, int center_y, int width, int height)
{
    int all_zero = x1 == 0 && y1 == 0 && x2 == 0 && y2 == 0 &&
                   center_x == 0 && center_y == 0;

    if (state != NETWORK_TRACKING && all_zero)
        return 1;
    return x1 >= 0 && y1 >= 0 && x2 >= x1 && y2 >= y1 &&
           x2 < width && y2 < height &&
           center_x >= x1 && center_x <= x2 &&
           center_y >= y1 && center_y <= y2;
}

static int fail_exchange(NetworkClient *client, int error)
{
    network_client_close(client);
    return error;
}

int network_client_exchange(NetworkClient *client, const uint16_t *rgb565,
                            int width, int height, int stride,
                            uint64_t timestamp_ms, const NetworkTouch *touch,
                            NetworkTrackResult *result)
{
    size_t image_size;
    uint32_t frame_id;
    uint32_t flags;
    uint8_t request_header[NETWORK_HEADER_SIZE];
    uint8_t request_meta[NETWORK_REQUEST_META_SIZE];
    uint8_t response_header[NETWORK_HEADER_SIZE];
    uint8_t response_body[NETWORK_RESPONSE_BODY_SIZE];
    uint8_t *out;
    const uint8_t *in;
    uint32_t magic;
    uint16_t version;
    uint16_t message_type;
    uint32_t body_size;
    uint32_t response_frame_id;
    uint32_t state;
    int x1, y1, x2, y2, center_x, center_y;
    uint32_t confidence;
    uint32_t server_error;
    NetworkTrackResult decoded;
    int rc;

    clear_result(result);
    if (client == NULL || rgb565 == NULL || result == NULL ||
        width <= 0 || height <= 0 || stride < width || stride <= 0 ||
        width > UINT16_MAX || height > UINT16_MAX || stride > UINT16_MAX)
        return NETWORK_ERR_ARGUMENT;
    if ((size_t)height > SIZE_MAX / (size_t)stride / sizeof(uint16_t))
        return NETWORK_ERR_ARGUMENT;
    image_size = (size_t)height * (size_t)stride * sizeof(uint16_t);
    if (image_size > NETWORK_MAX_IMAGE_SIZE)
        return NETWORK_ERR_ARGUMENT;
    if (touch != NULL && touch->valid &&
        (touch->x < 0 || touch->x >= width ||
         touch->y < 0 || touch->y >= height))
        return NETWORK_ERR_ARGUMENT;
    if (!client->initialized || client->socket_fd < 0)
        return NETWORK_ERR_ARGUMENT;

    frame_id = client->next_frame_id == 0 ? 1U : client->next_frame_id;
    flags = touch != NULL && touch->valid ? NETWORK_TOUCH_VALID : 0U;

    out = request_header;
    put_u32(&out, NETWORK_MAGIC);
    put_u16(&out, NETWORK_VERSION);
    put_u16(&out, NETWORK_MESSAGE_FRAME);
    put_u32(&out, NETWORK_REQUEST_META_SIZE + (uint32_t)image_size);

    out = request_meta;
    put_u32(&out, frame_id);
    put_u64(&out, timestamp_ms);
    put_u16(&out, (uint16_t)width);
    put_u16(&out, (uint16_t)height);
    put_u16(&out, (uint16_t)stride);
    put_u16(&out, NETWORK_PIXEL_RGB565_LE);
    put_u32(&out, flags);
    put_u32(&out, flags ? (uint32_t)touch->x : 0U);
    put_u32(&out, flags ? (uint32_t)touch->y : 0U);
    put_u32(&out, (uint32_t)image_size);

    rc = send_all(client->socket_fd, request_header, sizeof(request_header));
    if (rc == NETWORK_OK)
        rc = send_all(client->socket_fd, request_meta, sizeof(request_meta));
    if (rc == NETWORK_OK)
        rc = send_all(client->socket_fd, rgb565, image_size);
    if (rc != NETWORK_OK)
        return fail_exchange(client, rc);
    client->next_frame_id = frame_id == UINT32_MAX ? 1U : frame_id + 1U;

    rc = recv_all(client->socket_fd, response_header, sizeof(response_header));
    if (rc != NETWORK_OK)
        return fail_exchange(client, rc);

    in = response_header;
    magic = get_u32(&in);
    version = get_u16(&in);
    message_type = get_u16(&in);
    body_size = get_u32(&in);
    if (magic != NETWORK_MAGIC || version != NETWORK_VERSION ||
        message_type != NETWORK_MESSAGE_RESULT ||
        body_size != NETWORK_RESPONSE_BODY_SIZE)
        return fail_exchange(client, NETWORK_ERR_PROTOCOL);

    rc = recv_all(client->socket_fd, response_body, sizeof(response_body));
    if (rc != NETWORK_OK)
        return fail_exchange(client, rc);

    in = response_body;
    response_frame_id = get_u32(&in);
    state = get_u32(&in);
    x1 = (int32_t)get_u32(&in);
    y1 = (int32_t)get_u32(&in);
    x2 = (int32_t)get_u32(&in);
    y2 = (int32_t)get_u32(&in);
    center_x = (int32_t)get_u32(&in);
    center_y = (int32_t)get_u32(&in);
    confidence = get_u32(&in);
    server_error = get_u32(&in);

    if (response_frame_id != frame_id || state > NETWORK_TRACK_ERROR ||
        confidence > 1000U ||
        (server_error != 0U && state != NETWORK_TRACK_ERROR) ||
        !valid_box((int)state, x1, y1, x2, y2, center_x, center_y,
                   width, height))
        return fail_exchange(client, NETWORK_ERR_PROTOCOL);

    memset(&decoded, 0, sizeof(decoded));
    decoded.frame_id = response_frame_id;
    decoded.state = (NetworkTrackState)state;
    decoded.x1 = x1;
    decoded.y1 = y1;
    decoded.x2 = x2;
    decoded.y2 = y2;
    decoded.center_x = center_x;
    decoded.center_y = center_y;
    decoded.confidence_milli = (int)confidence;
    *result = decoded;

    return NETWORK_OK;
}
