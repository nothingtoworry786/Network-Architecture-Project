/*
 * bcurl - BH/1 client.
 *
 *     ./bcurl [-v] host:port/path [/path ...]
 *
 * Opens exactly one TCP connection, sends one GET per path over it, and
 * writes only the response bodies to stdout. With -v, an annotated hexdump of
 * every frame sent and received goes to stderr.
 *
 * Exit status: 0 every response was 2xx
 *              1 at least one response was not 2xx (4xx, 5xx, ...)
 *              2 usage, network or protocol error
 */
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "hexdump.h"
#include "net.h"
#include "protocol.h"

#define USER_AGENT "bcurl/1.0"

enum { EXIT_ALL_2XX = 0, EXIT_BAD_STATUS = 1, EXIT_ERROR = 2 };

static int connect_to(const char *host, const char *port)
{
    struct addrinfo hints, *list, *ai;
    int fd = -1;
    int err;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; /* the server listens on IPv4 */
    hints.ai_socktype = SOCK_STREAM;
    err = getaddrinfo(host, port, &hints, &list);
    if (err != 0) {
        fprintf(stderr, "bcurl: %s:%s: %s\n", host, port, gai_strerror(err));
        return -1;
    }
    /* Stops at the first address that accepts: never two live connections. */
    for (ai = list; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd >= 0 && connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        if (fd >= 0)
            close(fd);
        fd = -1;
    }
    if (fd < 0)
        perror("bcurl: connect");
    freeaddrinfo(list);
    return fd;
}

/* One request/response exchange on the open connection. Returns the status
 * code, or -1 if the exchange failed. */
static int fetch(int fd, const char *authority, const char *path, int verbose)
{
    uint8_t request[BH_HEADER_SIZE + BH_MAX_REQUEST_PAYLOAD];
    bh_request req = { .method = BH_METHOD_GET, .path = bh_str(path), .header_count = 2 };
    bh_response res;
    uint8_t *frame = NULL;
    size_t len, frame_len = 0;
    int rc, status = -1;

    req.headers[0] = (bh_header){ BH_HDR_HOST, { NULL, 0 }, bh_str(authority) };
    req.headers[1] = (bh_header){ BH_HDR_USER_AGENT, { NULL, 0 }, bh_str(USER_AGENT) };
    len = bh_encode_request(request, sizeof request, &req);
    if (len == 0) {
        fprintf(stderr, "bcurl: %s: path is not encodable (max %u bytes)\n", path,
                BH_MAX_PATH_LEN);
        return -1;
    }
    if (verbose)
        hexdump_frame(stderr, "request (client -> server)", request, len);
    if (write_full(fd, request, len) != 0) {
        perror("bcurl: send");
        return -1;
    }

    rc = recv_frame(fd, BH_TYPE_RESPONSE, BH_MAX_PAYLOAD, &frame, &frame_len, NULL);
    if (rc != RECV_OK) {
        fprintf(stderr, "bcurl: %s\n",
                rc == RECV_EOF  ? "server closed the connection without responding"
                : rc == RECV_IO ? "connection lost while reading the response"
                                : "malformed response frame");
        return -1;
    }
    if (verbose)
        hexdump_frame(stderr, "response (server -> client)", frame, frame_len);

    bh_reader r = { .buf = frame, .len = frame_len, .pos = BH_HEADER_SIZE };
    if (bh_decode_response(&r, &res) == 0) {
        /* The body is raw bytes: fwrite with an explicit length, never %s. */
        fwrite(res.body.data, 1, res.body.len, stdout);
        status = res.status;
        if (status < 200 || status > 299)
            fprintf(stderr, "bcurl: %s: status %d\n", path, status);
    } else {
        fprintf(stderr, "bcurl: malformed response payload\n");
    }
    free(frame);
    return status;
}

static int usage(void)
{
    fprintf(stderr, "usage: bcurl [-v] host:port/path [/path ...]\n");
    return EXIT_ERROR;
}

int main(int argc, char **argv)
{
    char authority[256], host[256];
    const char *url, *slash;
    char *colon;
    size_t authority_len;
    int verbose = 0, first = 1, exit_code = EXIT_ALL_2XX;
    int fd;

    if (first < argc && strcmp(argv[first], "-v") == 0) {
        verbose = 1;
        first++;
    }
    if (first >= argc)
        return usage();

    /* Split "host:port/path" into authority ("host:port") and path. */
    url = argv[first];
    slash = strchr(url, '/');
    authority_len = slash ? (size_t)(slash - url) : strlen(url);
    if (authority_len == 0 || authority_len >= sizeof authority)
        return usage();
    memcpy(authority, url, authority_len);
    authority[authority_len] = '\0';
    memcpy(host, authority, authority_len + 1);
    colon = strrchr(host, ':');
    if (colon == NULL || colon == host || colon[1] == '\0')
        return usage();
    *colon = '\0';
    for (int i = first + 1; i < argc; i++)
        if (argv[i][0] != '/')
            return usage();

    fd = connect_to(host, colon + 1);
    if (fd < 0)
        return EXIT_ERROR;

    /* Every path goes over this one connection: request, response, request... */
    for (int i = first; i < argc; i++) {
        int status = fetch(fd, authority, i == first ? (slash ? slash : "/") : argv[i], verbose);

        if (status < 0) {
            exit_code = EXIT_ERROR;
            break;
        }
        if (status < 200 || status > 299)
            exit_code = EXIT_BAD_STATUS;
    }
    close(fd);

    if (fflush(stdout) != 0) {
        perror("bcurl: stdout");
        return EXIT_ERROR;
    }
    return exit_code;
}
