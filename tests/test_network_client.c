#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "network_client.h"

ssize_t __real_send(int socket_fd, const void *buffer, size_t length, int flags);
ssize_t __real_recv(int socket_fd, void *buffer, size_t length, int flags);

static int injected_fd = -1;
static int send_eintr_pending;
static int recv_eintr_pending;

ssize_t __wrap_send(int socket_fd, const void *buffer, size_t length, int flags)
{
    if (socket_fd == injected_fd) {
        if (send_eintr_pending) {
            send_eintr_pending = 0;
            errno = EINTR;
            return -1;
        }
        if (length > 3)
            length = 3;
    }
    return __real_send(socket_fd, buffer, length, flags);
}

ssize_t __wrap_recv(int socket_fd, void *buffer, size_t length, int flags)
{
    if (socket_fd == injected_fd) {
        if (recv_eintr_pending) {
            recv_eintr_pending = 0;
            errno = EINTR;
            return -1;
        }
        if (length > 3)
            length = 3;
    }
    return __real_recv(socket_fd, buffer, length, flags);
}

static void test_close_and_argument_validation(void)
{
    NetworkClient client = NETWORK_CLIENT_INITIALIZER;
    NetworkClient zero_client = {0};
    NetworkTrackResult result;
    NetworkTouch touch;
    uint16_t pixel = 0;
    uint16_t small_image[4] = {0};
    int stdin_pipe[2];
    int saved_stdin;
    int sockets[2];

    assert(pipe(stdin_pipe) == 0);
    saved_stdin = dup(STDIN_FILENO);
    assert(dup2(stdin_pipe[0], STDIN_FILENO) == STDIN_FILENO);
    network_client_close(&zero_client);
    assert(fcntl(STDIN_FILENO, F_GETFD) >= 0);
    if (saved_stdin >= 0) {
        assert(dup2(saved_stdin, STDIN_FILENO) == STDIN_FILENO);
        close(saved_stdin);
    } else {
        close(STDIN_FILENO);
    }
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);

    network_client_close(&client);
    network_client_close(&client);
    assert(client.socket_fd == -1);

    assert(network_client_exchange(NULL, &pixel, 1, 1, 1, 0,
                                   NULL, &result) == NETWORK_ERR_ARGUMENT);
    assert(network_client_exchange(&client, NULL, 1, 1, 1, 0,
                                   NULL, &result) == NETWORK_ERR_ARGUMENT);
    assert(network_client_exchange(&client, &pixel, 1, 1, 0, 0,
                                   NULL, &result) == NETWORK_ERR_ARGUMENT);
    assert(network_client_exchange(&client, &pixel, 1, 1, 1, 0,
                                   NULL, NULL) == NETWORK_ERR_ARGUMENT);

    touch.valid = 1;
    touch.x = 1;
    touch.y = 0;
    assert(network_client_exchange(&client, &pixel, 1, 1, 1, 0,
                                   &touch, &result) == NETWORK_ERR_ARGUMENT);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    client.socket_fd = sockets[0];
    client.next_frame_id = 1;
    client.timeout_ms = 100;
    client.initialized = 1;
    assert(network_client_exchange(&client, small_image, 0, 2, 2, 0,
                                   NULL, &result) == NETWORK_ERR_ARGUMENT);
    assert(network_client_exchange(&client, small_image, -1, 2, 2, 0,
                                   NULL, &result) == NETWORK_ERR_ARGUMENT);
    assert(network_client_exchange(&client, small_image, 2, 0, 2, 0,
                                   NULL, &result) == NETWORK_ERR_ARGUMENT);
    assert(network_client_exchange(&client, small_image, 2, 2, 1, 0,
                                   NULL, &result) == NETWORK_ERR_ARGUMENT);
    assert(network_client_exchange(&client, small_image, 65536, 1, 65536, 0,
                                   NULL, &result) == NETWORK_ERR_ARGUMENT);
    assert(network_client_exchange(&client, small_image, 2048, 1024, 2048, 0,
                                   NULL, &result) == NETWORK_ERR_ARGUMENT);
    assert(network_client_exchange(&client, small_image, 65535, 65535, 65535, 0,
                                   NULL, &result) == NETWORK_ERR_ARGUMENT);
    network_client_close(&client);
    close(sockets[1]);
}

typedef struct {
    int fd;
    int expect_touch;
    uint32_t expected_frame_id;
} ServerArgs;

static void read_exact(int fd, void *data, size_t size)
{
    unsigned char *cursor = data;
    while (size > 0) {
        ssize_t count = recv(fd, cursor, size, 0);
        assert(count > 0);
        cursor += count;
        size -= (size_t)count;
    }
}

static void write_exact(int fd, const void *data, size_t size)
{
    const unsigned char *cursor = data;
    while (size > 0) {
        ssize_t count = send(fd, cursor, size, MSG_NOSIGNAL);
        assert(count > 0);
        cursor += count;
        size -= (size_t)count;
    }
}

static uint16_t read_u16(const unsigned char **cursor)
{
    uint16_t net;
    memcpy(&net, *cursor, sizeof(net));
    *cursor += sizeof(net);
    return ntohs(net);
}

static uint32_t read_u32(const unsigned char **cursor)
{
    uint32_t net;
    memcpy(&net, *cursor, sizeof(net));
    *cursor += sizeof(net);
    return ntohl(net);
}

static uint64_t read_u64(const unsigned char **cursor)
{
    uint64_t high = read_u32(cursor);
    uint64_t low = read_u32(cursor);
    return (high << 32) | low;
}

static void write_u16(unsigned char **cursor, uint16_t value)
{
    uint16_t net = htons(value);
    memcpy(*cursor, &net, sizeof(net));
    *cursor += sizeof(net);
}

static void write_u32(unsigned char **cursor, uint32_t value)
{
    uint32_t net = htonl(value);
    memcpy(*cursor, &net, sizeof(net));
    *cursor += sizeof(net);
}

static void *successful_server(void *argument)
{
    ServerArgs *args = argument;
    unsigned char header[12];
    unsigned char metadata[36];
    unsigned char image[8];
    unsigned char response_header[12];
    unsigned char response_body[40];
    const unsigned char *in;
    unsigned char *out;

    read_exact(args->fd, header, sizeof(header));
    in = header;
    assert(read_u32(&in) == 0x5654524bU);
    assert(read_u16(&in) == 1);
    assert(read_u16(&in) == 1);
    assert(read_u32(&in) == 44);

    read_exact(args->fd, metadata, sizeof(metadata));
    in = metadata;
    assert(read_u32(&in) == args->expected_frame_id);
    assert(read_u64(&in) == 1234);
    assert(read_u16(&in) == 2);
    assert(read_u16(&in) == 2);
    assert(read_u16(&in) == 2);
    assert(read_u16(&in) == 1);
    assert(read_u32(&in) == (uint32_t)args->expect_touch);
    assert(read_u32(&in) == (args->expect_touch ? 1U : 0U));
    assert(read_u32(&in) == 0);
    assert(read_u32(&in) == 8);

    read_exact(args->fd, image, sizeof(image));
    assert(image[0] == 0x34 && image[1] == 0x12);
    assert(image[2] == 0xcd && image[3] == 0xab);
    assert(image[4] == 0x01 && image[5] == 0x00);
    assert(image[6] == 0xff && image[7] == 0x00);

    out = response_header;
    write_u32(&out, 0x5654524bU);
    write_u16(&out, 1);
    write_u16(&out, 2);
    write_u32(&out, 40);

    out = response_body;
    write_u32(&out, args->expected_frame_id);
    write_u32(&out, args->expect_touch ? NETWORK_TRACKING : NETWORK_TRACK_IDLE);
    write_u32(&out, 0);
    write_u32(&out, 0);
    write_u32(&out, args->expect_touch ? 1U : 0U);
    write_u32(&out, args->expect_touch ? 1U : 0U);
    write_u32(&out, 0);
    write_u32(&out, 0);
    write_u32(&out, args->expect_touch ? 900U : 0U);
    write_u32(&out, 0);
    write_exact(args->fd, response_header, sizeof(response_header));
    write_exact(args->fd, response_body, sizeof(response_body));
    close(args->fd);
    return NULL;
}

static void test_successful_exchange(void)
{
    int sockets[2];
    pthread_t thread;
    ServerArgs args;
    NetworkClient client;
    NetworkTouch touch = {1, 1, 0};
    NetworkTrackResult result;
    uint16_t image[4] = {0x1234, 0xabcd, 0x0001, 0x00ff};

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    args.fd = sockets[1];
    args.expect_touch = 1;
    args.expected_frame_id = 1;
    assert(pthread_create(&thread, NULL, successful_server, &args) == 0);

    client.socket_fd = sockets[0];
    client.next_frame_id = 1;
    client.timeout_ms = 1000;
    client.initialized = 1;
    assert(network_client_exchange(&client, image, 2, 2, 2, 1234,
                                   &touch, &result) == NETWORK_OK);
    assert(result.frame_id == 1);
    assert(result.state == NETWORK_TRACKING);
    assert(result.x1 == 0 && result.y1 == 0);
    assert(result.x2 == 1 && result.y2 == 1);
    assert(result.center_x == 0 && result.center_y == 0);
    assert(result.confidence_milli == 900);
    assert(client.next_frame_id == 2);
    assert(pthread_join(thread, NULL) == 0);
    network_client_close(&client);
}

static void test_no_touch_exchange(void)
{
    int sockets[2];
    pthread_t thread;
    ServerArgs args;
    NetworkClient client;
    NetworkTrackResult result;
    uint16_t image[4] = {0x1234, 0xabcd, 0x0001, 0x00ff};

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    args.fd = sockets[1];
    args.expect_touch = 0;
    args.expected_frame_id = 2;
    assert(pthread_create(&thread, NULL, successful_server, &args) == 0);

    client.socket_fd = sockets[0];
    client.next_frame_id = 2;
    client.timeout_ms = 1000;
    client.initialized = 1;
    assert(network_client_exchange(&client, image, 2, 2, 2, 1234,
                                   NULL, &result) == NETWORK_OK);
    assert(result.frame_id == 2);
    assert(result.state == NETWORK_TRACK_IDLE);
    assert(client.next_frame_id == 3);
    assert(pthread_join(thread, NULL) == 0);
    network_client_close(&client);
}

static void *accept_once(void *argument)
{
    int listener = *(int *)argument;
    int connection = accept(listener, NULL, NULL);
    assert(connection >= 0);
    close(connection);
    return NULL;
}

static void test_client_initialization(void)
{
    NetworkClient client = NETWORK_CLIENT_INITIALIZER;
    struct sockaddr_in address;
    socklen_t address_size = sizeof(address);
    struct timeval send_timeout;
    struct timeval receive_timeout;
    socklen_t option_size;
    pthread_t thread;
    int listener;

    assert(network_client_init(NULL, "127.0.0.1", 9000, 100) ==
           NETWORK_ERR_ARGUMENT);
    assert(network_client_init(&client, NULL, 9000, 100) ==
           NETWORK_ERR_ARGUMENT);
    assert(client.socket_fd == -1);
    assert(network_client_init(&client, "127.0.0.1", 0, 100) ==
           NETWORK_ERR_ARGUMENT);
    assert(network_client_init(&client, "127.0.0.1", 9000, 0) ==
           NETWORK_ERR_ARGUMENT);
    assert(network_client_init(&client, "not-an-ip", 9000, 100) ==
           NETWORK_ERR_ARGUMENT);

    listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(listener, 1) == 0);
    assert(getsockname(listener, (struct sockaddr *)&address, &address_size) == 0);
    assert(pthread_create(&thread, NULL, accept_once, &listener) == 0);

    assert(network_client_init(&client, "127.0.0.1",
                               ntohs(address.sin_port), 250) == NETWORK_OK);
    assert(client.socket_fd >= 0);
    assert(client.next_frame_id == 1);
    assert(client.timeout_ms == 250);
    option_size = sizeof(send_timeout);
    assert(getsockopt(client.socket_fd, SOL_SOCKET, SO_SNDTIMEO,
                      &send_timeout, &option_size) == 0);
    option_size = sizeof(receive_timeout);
    assert(getsockopt(client.socket_fd, SOL_SOCKET, SO_RCVTIMEO,
                      &receive_timeout, &option_size) == 0);
    assert(send_timeout.tv_sec == 0 && send_timeout.tv_usec >= 250000 &&
           send_timeout.tv_usec <= 260000);
    assert(receive_timeout.tv_sec == 0 && receive_timeout.tv_usec >= 250000 &&
           receive_timeout.tv_usec <= 260000);

    {
        int live_fd = client.socket_fd;
        assert(network_client_init(&client, "127.0.0.1",
                                   ntohs(address.sin_port), 250) ==
               NETWORK_ERR_ARGUMENT);
        assert(client.socket_fd == live_fd);
    }

    network_client_close(&client);
    assert(pthread_join(thread, NULL) == 0);
    close(listener);

    listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    address_size = sizeof(address);
    assert(getsockname(listener, (struct sockaddr *)&address, &address_size) == 0);
    close(listener);
    assert(network_client_init(&client, "127.0.0.1",
                               ntohs(address.sin_port), 100) ==
           NETWORK_ERR_CONNECT);
    assert(client.socket_fd == -1);
}

typedef enum {
    BAD_MAGIC,
    BAD_VERSION,
    BAD_TYPE,
    BAD_BODY_SIZE,
    BAD_FRAME_ID,
    BAD_STATE,
    BAD_CONFIDENCE,
    BAD_BOX,
    BAD_CENTER,
    BAD_ERROR_STATE,
    PEER_CLOSE,
    SILENT_PEER
} FailureKind;

typedef struct {
    int fd;
    FailureKind kind;
} FailureArgs;

static void consume_request(int fd)
{
    unsigned char header[12];
    unsigned char metadata[36];
    unsigned char image[8];
    read_exact(fd, header, sizeof(header));
    read_exact(fd, metadata, sizeof(metadata));
    read_exact(fd, image, sizeof(image));
}

static void *failure_server(void *argument)
{
    FailureArgs *args = argument;
    unsigned char header[12];
    unsigned char body[40];
    unsigned char *out;
    uint32_t magic = args->kind == BAD_MAGIC ? 0U : 0x5654524bU;
    uint16_t version = args->kind == BAD_VERSION ? 2U : 1U;
    uint16_t type = args->kind == BAD_TYPE ? 1U : 2U;
    uint32_t body_size = args->kind == BAD_BODY_SIZE ? 39U : 40U;
    uint32_t frame_id = args->kind == BAD_FRAME_ID ? 2U : 1U;
    uint32_t state = args->kind == BAD_STATE ? 4U : NETWORK_TRACKING;
    uint32_t confidence = args->kind == BAD_CONFIDENCE ? 1001U : 900U;
    uint32_t x1 = args->kind == BAD_BOX ? 2U : 0U;
    uint32_t x2 = 1U;
    uint32_t center_x = args->kind == BAD_CENTER ? 2U : 0U;
    uint32_t server_error = args->kind == BAD_ERROR_STATE ? 7U : 0U;
    struct timespec delay = {0, 200000000L};

    consume_request(args->fd);
    if (args->kind == PEER_CLOSE) {
        close(args->fd);
        return NULL;
    }
    if (args->kind == SILENT_PEER) {
        nanosleep(&delay, NULL);
        close(args->fd);
        return NULL;
    }

    out = header;
    write_u32(&out, magic);
    write_u16(&out, version);
    write_u16(&out, type);
    write_u32(&out, body_size);
    out = body;
    write_u32(&out, frame_id);
    write_u32(&out, state);
    write_u32(&out, x1);
    write_u32(&out, 0);
    write_u32(&out, x2);
    write_u32(&out, 1);
    write_u32(&out, center_x);
    write_u32(&out, 0);
    write_u32(&out, confidence);
    write_u32(&out, server_error);
    write_exact(args->fd, header, sizeof(header));
    if (magic == 0x5654524bU && version == 1U && type == 2U &&
        body_size == 40U)
        write_exact(args->fd, body, sizeof(body));
    close(args->fd);
    return NULL;
}

static int run_failure_case(FailureKind kind)
{
    int sockets[2];
    pthread_t thread;
    FailureArgs args;
    NetworkClient client;
    NetworkTrackResult result;
    uint16_t image[4] = {0};
    struct timeval timeout = {0, 50000};
    int rc;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    args.fd = sockets[1];
    args.kind = kind;
    assert(pthread_create(&thread, NULL, failure_server, &args) == 0);
    client.socket_fd = sockets[0];
    client.next_frame_id = 1;
    client.timeout_ms = 50;
    client.initialized = 1;
    if (kind == SILENT_PEER)
        assert(setsockopt(client.socket_fd, SOL_SOCKET, SO_RCVTIMEO,
                          &timeout, sizeof(timeout)) == 0);

    rc = network_client_exchange(&client, image, 2, 2, 2, 1, NULL, &result);
    assert(result.state == NETWORK_TRACK_ERROR);
    assert(client.socket_fd == -1);
    assert(client.next_frame_id == 2);
    assert(pthread_join(thread, NULL) == 0);
    return rc;
}

static void *server_error_server(void *argument)
{
    int fd = *(int *)argument;
    unsigned char header[12];
    unsigned char body[40];
    unsigned char *out;

    consume_request(fd);
    out = header;
    write_u32(&out, 0x5654524bU);
    write_u16(&out, 1);
    write_u16(&out, 2);
    write_u32(&out, 40);
    out = body;
    write_u32(&out, 1);
    write_u32(&out, NETWORK_TRACK_ERROR);
    write_u32(&out, 0);
    write_u32(&out, 0);
    write_u32(&out, 0);
    write_u32(&out, 0);
    write_u32(&out, 0);
    write_u32(&out, 0);
    write_u32(&out, 0);
    write_u32(&out, 7);
    write_exact(fd, header, sizeof(header));
    write_exact(fd, body, sizeof(body));
    close(fd);
    return NULL;
}

static void test_server_processing_error(void)
{
    int sockets[2];
    pthread_t thread;
    NetworkClient client = NETWORK_CLIENT_INITIALIZER;
    NetworkTrackResult result;
    uint16_t image[4] = {0};

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(pthread_create(&thread, NULL, server_error_server, &sockets[1]) == 0);
    client.socket_fd = sockets[0];
    client.timeout_ms = 100;
    client.initialized = 1;
    assert(network_client_exchange(&client, image, 2, 2, 2, 1,
                                   NULL, &result) == NETWORK_OK);
    assert(result.state == NETWORK_TRACK_ERROR);
    assert(client.next_frame_id == 2);
    assert(pthread_join(thread, NULL) == 0);
    network_client_close(&client);
}

static void test_protocol_and_io_failures(void)
{
    FailureKind malformed[] = {
        BAD_MAGIC, BAD_VERSION, BAD_TYPE, BAD_BODY_SIZE, BAD_FRAME_ID,
        BAD_STATE, BAD_CONFIDENCE, BAD_BOX, BAD_CENTER, BAD_ERROR_STATE
    };
    size_t i;

    for (i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i)
        assert(run_failure_case(malformed[i]) == NETWORK_ERR_PROTOCOL);
    assert(run_failure_case(PEER_CLOSE) == NETWORK_ERR_IO);
    assert(run_failure_case(SILENT_PEER) == NETWORK_ERR_TIMEOUT);
}

static void test_short_io_and_eintr(void)
{
    int sockets[2];
    pthread_t thread;
    ServerArgs args;
    NetworkClient client;
    NetworkTouch touch = {1, 1, 0};
    NetworkTrackResult result;
    uint16_t image[4] = {0x1234, 0xabcd, 0x0001, 0x00ff};

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    args.fd = sockets[1];
    args.expect_touch = 1;
    args.expected_frame_id = 1;
    assert(pthread_create(&thread, NULL, successful_server, &args) == 0);

    client.socket_fd = sockets[0];
    client.next_frame_id = 1;
    client.timeout_ms = 1000;
    client.initialized = 1;
    injected_fd = client.socket_fd;
    send_eintr_pending = 1;
    recv_eintr_pending = 1;
    assert(network_client_exchange(&client, image, 2, 2, 2, 1234,
                                   &touch, &result) == NETWORK_OK);
    assert(send_eintr_pending == 0);
    assert(recv_eintr_pending == 0);
    assert(result.state == NETWORK_TRACKING);
    injected_fd = -1;

    assert(pthread_join(thread, NULL) == 0);
    network_client_close(&client);
}

int main(void)
{
    test_close_and_argument_validation();
    test_successful_exchange();
    test_no_touch_exchange();
    test_client_initialization();
    test_protocol_and_io_failures();
    test_server_processing_error();
    test_short_io_and_eintr();
    return 0;
}
