/*
 * observe - BH/1 file server.
 *
 *     ./observe <document_root> <port>
 *
 * Each connection carries any number of request/response exchanges and stays
 * open until the client closes it (SPEC 4).
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "net.h"
#include "protocol.h"

#define SERVER_NAME      "observe/1.0"
#define LISTEN_BACKLOG   16
#define IDLE_TIMEOUT_SEC 30

struct file {
    uint8_t *data; /* malloc'd; caller frees */
    size_t len;
    const char *type;
};

static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_listener = -1;

static void on_stop_signal(int sig)
{
    (void)sig;
    g_stop = 1;
    /* Closing the listener makes a blocked *or imminent* accept() fail, so
     * there is no window in which the signal can be missed. */
    close(g_listener);
}

/* ------------------------------------------------------------ file lookup */

/*
 * Lexical path check (SPEC 13). Rejects NUL (would truncate the C string the
 * kernel sees), backslash (a separator on some systems), and any ".."
 * segment. There is no percent-decoding in BH/1, so "%2e%2e" is just an
 * ordinary file name and cannot become ".." later.
 */
static int path_is_safe(bh_bytes path)
{
    size_t segment = 0; /* index where the current segment starts */

    for (size_t i = 0; i <= path.len; i++) {
        if (i < path.len && (path.data[i] == '\0' || path.data[i] == '\\'))
            return 0;
        if (i == path.len || path.data[i] == '/') {
            if (i - segment == 2 && path.data[segment] == '.' && path.data[segment + 1] == '.')
                return 0;
            segment = i + 1;
        }
    }
    return 1;
}

static const char *content_type_for(const char *path)
{
    static const struct {
        const char *ext;
        const char *type;
    } types[] = {
        { ".html", "text/html" },
        { ".txt", "text/plain" },
    };
    const char *dot = strrchr(path, '.');

    for (size_t i = 0; dot != NULL && i < sizeof types / sizeof types[0]; i++)
        if (strcmp(dot, types[i].ext) == 0)
            return types[i].type;
    return "application/octet-stream";
}

static uint16_t read_regular_file(int fd, struct file *out)
{
    struct stat st;
    size_t size, got = 0;
    uint8_t *buf;

    if (fstat(fd, &st) != 0)
        return BH_STATUS_SERVER_ERROR;
    if (!S_ISREG(st.st_mode))
        return BH_STATUS_NOT_FOUND; /* directories, FIFOs, devices: not files */
    /* ponytail: whole file in one frame, so files above ~16 MiB get 500.
     * Add a chunked/continuation frame type if larger files matter. */
    if (st.st_size < 0 || (uintmax_t)st.st_size > BH_MAX_BODY_LEN)
        return BH_STATUS_SERVER_ERROR;

    size = (size_t)st.st_size;
    buf = malloc(size + 1); /* +1 so an empty file is not malloc(0) */
    if (buf == NULL)
        return BH_STATUS_SERVER_ERROR;
    while (got < size) {
        ssize_t r = read(fd, buf + got, size - got);

        if (r > 0) {
            got += (size_t)r;
        } else if (r == 0) {
            break; /* file shrank under us: serve what exists */
        } else if (errno != EINTR) {
            free(buf);
            return BH_STATUS_SERVER_ERROR;
        }
    }
    out->data = buf;
    out->len = got;
    return BH_STATUS_OK;
}

/* Maps a (lexically safe) request path to a file under root and loads it. */
static uint16_t load_file(const char *root, bh_bytes path, struct file *out)
{
    char joined[PATH_MAX], resolved[PATH_MAX];
    size_t root_len = strlen(root);
    uint16_t status;
    int n, fd;

    n = snprintf(joined, sizeof joined, "%s%.*s", root, (int)path.len, (const char *)path.data);
    if (n < 0 || (size_t)n >= sizeof joined || realpath(joined, resolved) == NULL)
        return BH_STATUS_NOT_FOUND;
    /* Second line of defence: after symlinks are resolved the file must
     * still be strictly inside the root. */
    if (strncmp(resolved, root, root_len) != 0 || resolved[root_len] != '/')
        return BH_STATUS_NOT_FOUND;

    /* O_NONBLOCK: opening a FIFO must not hang the server. No effect on
     * regular files. */
    fd = open(resolved, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? BH_STATUS_NOT_FOUND : BH_STATUS_SERVER_ERROR;
    status = read_regular_file(fd, out);
    close(fd);
    out->type = content_type_for(resolved);
    return status;
}

/* -------------------------------------------------------------- responses */

static const char *reason(uint16_t status)
{
    switch (status) {
    case BH_STATUS_OK:          return "OK";
    case BH_STATUS_BAD_REQUEST: return "Bad Request";
    case BH_STATUS_NOT_FOUND:   return "Not Found";
    default:                    return "Internal Server Error";
    }
}

/* Builds one RESPONSE frame and sends all of it. `seq` is the number of this
 * response on the connection, reported in a custom header. */
static int send_response(int fd, uint16_t status, const char *type, bh_bytes body, unsigned seq)
{
    char seq_text[16];
    bh_response res = { .status = status, .header_count = 3, .body = body };
    size_t cap = BH_HEADER_SIZE + BH_MAX_RESPONSE_HEAD + body.len;
    uint8_t *frame = malloc(cap);
    size_t len;
    int rc = -1;

    if (frame == NULL)
        return -1;
    snprintf(seq_text, sizeof seq_text, "%u", seq);
    res.headers[0] = (bh_header){ BH_HDR_CONTENT_TYPE, { NULL, 0 }, bh_str(type) };
    res.headers[1] = (bh_header){ BH_HDR_SERVER, { NULL, 0 }, bh_str(SERVER_NAME) };
    res.headers[2] = (bh_header){ BH_HDR_CUSTOM, bh_str("X-Request-Number"), bh_str(seq_text) };

    len = bh_encode_response(frame, cap, &res);
    if (len > 0)
        rc = write_full(fd, frame, len);
    free(frame);
    return rc;
}

static int send_error(int fd, uint16_t status, unsigned seq)
{
    char text[64];
    int n = snprintf(text, sizeof text, "%u %s\n", (unsigned)status, reason(status));
    bh_bytes body = { (const uint8_t *)text, (size_t)n };

    return send_response(fd, status, "text/plain", body, seq);
}

/* ------------------------------------------------------------- connection */

static void log_bytes(bh_bytes b)
{
    for (size_t i = 0; i < b.len; i++)
        fputc(isprint(b.data[i]) ? b.data[i] : '?', stderr); /* no raw bytes into the log */
}

static void serve_connection(int fd, const char *root, unsigned conn)
{
    unsigned served = 0;

    /* One iteration per request. The loop only ends when the client closes
     * the connection or the byte stream can no longer be trusted. */
    for (;;) {
        uint8_t *frame = NULL;
        size_t frame_len = 0;
        unsigned skipped = 0;
        struct file file = { NULL, 0, NULL };
        uint16_t status = BH_STATUS_BAD_REQUEST;
        bh_request req;
        int parsed = 0, sent;
        int rc = recv_frame(fd, BH_TYPE_REQUEST, BH_MAX_REQUEST_PAYLOAD, &frame, &frame_len,
                            &skipped);

        if (skipped > 0)
            fprintf(stderr, "observe: [conn %u] skipped %u unknown frame(s)\n", conn, skipped);
        if (rc == RECV_EOF || rc == RECV_IO)
            break;

        served++;
        if (rc == RECV_OK) {
            bh_reader r = { .buf = frame, .len = frame_len, .pos = BH_HEADER_SIZE };

            parsed = bh_decode_request(&r, &req) == 0;
            if (parsed && path_is_safe(req.path))
                status = load_file(root, req.path, &file);
        }

        fprintf(stderr, "observe: [conn %u] #%u ", conn, served);
        if (parsed) {
            fputs("GET ", stderr);
            log_bytes(req.path);
        } else {
            fputs("malformed request", stderr);
        }
        fprintf(stderr, " -> %u\n", (unsigned)status);

        if (status == BH_STATUS_OK)
            sent = send_response(fd, status, file.type, (bh_bytes){ file.data, file.len }, served);
        else
            sent = send_error(fd, status, served);
        free(file.data);
        free(frame);

        /* After a bad magic/version we cannot tell where the next frame
         * starts, so the 400 above is the last thing sent (SPEC 12). */
        if (sent != 0 || rc == RECV_DESYNC)
            break;
    }
    fprintf(stderr, "observe: [conn %u] closed after %u request(s)\n", conn, served);
}

/* ------------------------------------------------------------------ setup */

static int listen_on(const char *port_text)
{
    struct sockaddr_in addr;
    char *end;
    long port = strtol(port_text, &end, 10);
    int yes = 1;
    int fd;

    if (*port_text == '\0' || *end != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "observe: invalid port '%s'\n", port_text);
        return -1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    /* SO_REUSEADDR: allow a restart while old connections sit in TIME_WAIT. */
    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) != 0 ||
        bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(fd, LISTEN_BACKLOG) != 0) {
        perror("observe: cannot listen");
        if (fd >= 0)
            close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv)
{
    char root[PATH_MAX];
    struct stat st;
    struct sigaction sa;
    struct timeval idle = { IDLE_TIMEOUT_SEC, 0 };
    unsigned conn = 0;
    int listener;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <document_root> <port>\n", argv[0]);
        return 2;
    }
    if (realpath(argv[1], root) == NULL || stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "observe: %s: not a usable directory\n", argv[1]);
        return 2;
    }
    if (strcmp(root, "/") == 0)
        root[0] = '\0'; /* so root + "/path" never produces "//path" */

    listener = listen_on(argv[2]);
    if (listener < 0)
        return 2;
    g_listener = listener;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop_signal; /* no SA_RESTART: accept() must return */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    fprintf(stderr, "observe: serving %s, listening on port %s\n", argv[1], argv[2]);

    /* ponytail: one connection at a time, no threads. A second client waits
     * in the listen backlog. Fork per connection or poll() if concurrency
     * is ever needed. */
    while (!g_stop) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof peer;
        char ip[INET_ADDRSTRLEN] = "?";
        int fd = accept(listener, (struct sockaddr *)&peer, &peer_len);

        if (fd < 0) {
            if (g_stop)
                break;
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            perror("observe: accept");
            break;
        }
        /* An idle client, or one that stops reading its responses, must not
         * hold the only service slot forever: recv()/send() fail after the
         * timeout and we hang up. */
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &idle, sizeof idle);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &idle, sizeof idle);
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        conn++;
        fprintf(stderr, "observe: [conn %u] accepted %s:%u\n", conn, ip,
                (unsigned)ntohs(peer.sin_port));
        serve_connection(fd, root, conn);
        close(fd);
    }

    if (!g_stop)
        close(listener);
    fprintf(stderr, "observe: shut down\n");
    return 0;
}
