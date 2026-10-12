#define _POSIX_C_SOURCE 200809L
#include "lab.h"

#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Layer 3: all files, sockets and real clocks are confined to this adapter. */
struct options {
    bool sending;
    const char *session;
    const char *host;
    const char *file;
    unsigned window;
    unsigned timeout_ms;
    unsigned port;
    const char *loss, *corrupt, *duplicate;
};

static void usage(FILE *out)
{
    fprintf(out,
        "Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss]\n"
        "                  [-c corrupt] [-d dup] [-p port] <relay> <file>\n"
        "       myapp recv -s <session> [-p port] <relay> <file>\n"
        "Defaults: window=8, timeout-ms=250, loss=corrupt=dup=0, port=4250\n"
        "Session: 1-32 lowercase letters, digits, or hyphens. Window: 1-64.\n"
        "Probabilities: decimal 0 through 0.5 (no exponents). File size: at most 16 MiB.\n");
}

static bool integer_option(const char *text, unsigned maximum, unsigned *value)
{
    char *end;
    if (*text < '0' || *text > '9') return false;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed == 0 || parsed > maximum)
        return false;
    *value = (unsigned)parsed;
    return true;
}

static bool probability_option(const char *text, const char **value)
{
    /* Match the instructor relay's decimal grammar; retain the text so small
       rates never become scientific notation in HELLO. */
    const char *cursor = text;
    while (*cursor >= '0' && *cursor <= '9') ++cursor;
    if (*cursor == '.') {
        ++cursor;
        if (*cursor < '0' || *cursor > '9') return false;
        while (*cursor >= '0' && *cursor <= '9') ++cursor;
    }
    if (cursor == text || *cursor != '\0') return false;
    errno = 0;
    /* The grammar above guarantees a complete, nonnegative decimal. */
    double parsed = strtod(text, NULL);
    if (errno != 0 || parsed > 0.5) return false;
    *value = text;
    return true;
}

static bool session_valid(const char *session)
{
    if (session == NULL) return false;
    size_t length = strlen(session);
    if (length == 0 || length > 32) return false;
    for (size_t i = 0; i < length; ++i) {
        char c = session[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    }
    return true;
}

static bool parse_options(int argc, char **argv, struct options *options)
{
    *options = (struct options){.window = 8, .timeout_ms = 250, .port = 4250,
                                .loss = "0", .corrupt = "0", .duplicate = "0"};
    if (strcmp(argv[1], "send") == 0) options->sending = true;
    else if (strcmp(argv[1], "recv") != 0) return false;
    optind = 2;
    opterr = 0;
    int flag;
    while ((flag = getopt(argc, argv, "s:p:w:T:l:c:d:")) != -1) {
        if (!options->sending && flag != 's' && flag != 'p') return false;
        switch (flag) {
        case 's': options->session = optarg; break;
        case 'p':
            if (!integer_option(optarg, 65535, &options->port)) return false;
            break;
        case 'w':
            if (!integer_option(optarg, RDT_MAX_WINDOW, &options->window)) return false;
            break;
        case 'T':
            if (!integer_option(optarg, INT_MAX, &options->timeout_ms)) return false;
            break;
        case 'l':
            if (!probability_option(optarg, &options->loss)) return false;
            break;
        case 'c':
            if (!probability_option(optarg, &options->corrupt)) return false;
            break;
        case 'd':
            if (!probability_option(optarg, &options->duplicate)) return false;
            break;
        default: return false;
        }
    }
    if (argc - optind != 2 || !session_valid(options->session)) return false;
    options->host = argv[optind];
    options->file = argv[optind + 1];
    return options->host[0] != '\0' && options->file[0] != '\0';
}

static bool monotonic_ms(uint64_t *now)
{
    struct timespec stamp;
    if (clock_gettime(CLOCK_MONOTONIC, &stamp) != 0) {
        perror("clock_gettime");
        return false;
    }
    *now = (uint64_t)stamp.tv_sec * UINT64_C(1000) +
           (uint64_t)stamp.tv_nsec / UINT64_C(1000000);
    return true;
}

/* Return 1 for readable, 0 when due, and -1 for an I/O failure. */
static int wait_until(int socket_fd, uint64_t deadline)
{
    for (;;) {
        uint64_t now;
        if (!monotonic_ms(&now)) return -1;
        if (now >= deadline) return 0;
        uint64_t remaining = deadline - now;
        int timeout = remaining > INT_MAX ? INT_MAX : (int)remaining;
        struct pollfd descriptor = {.fd = socket_fd, .events = POLLIN};
        int ready = poll(&descriptor, 1, timeout);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { perror("poll"); return -1; }
        if (ready == 0) continue;
        if ((descriptor.revents & POLLIN) != 0) return 1;
        fprintf(stderr, "UDP socket failed (poll events %d).\n", descriptor.revents);
        return -1;
    }
}

static int connect_relay(const struct options *options)
{
    struct addrinfo hints = {0}, *addresses = NULL;
    char service[6];
    snprintf(service, sizeof service, "%u", options->port);
    /* The assignment's relay listens on IPv4 loopback. Resolve names as well. */
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    int error = getaddrinfo(options->host, service, &hints, &addresses);
    if (error != 0) {
        fprintf(stderr, "Cannot resolve relay '%s': %s\n", options->host, gai_strerror(error));
        return -1;
    }
    int socket_fd = -1;
    for (const struct addrinfo *address = addresses; address != NULL; address = address->ai_next) {
        socket_fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket_fd < 0) continue;
        if (connect(socket_fd, address->ai_addr, address->ai_addrlen) == 0) break;
        close(socket_fd);
        socket_fd = -1;
    }
    freeaddrinfo(addresses);
    if (socket_fd < 0) perror("connect relay");
    return socket_fd;
}

static bool send_bytes(int socket_fd, const uint8_t *bytes, size_t length)
{
    ssize_t sent;
    do { sent = send(socket_fd, bytes, length, 0); } while (sent < 0 && errno == EINTR);
    if (sent < 0) { perror("send"); return false; }
    if ((size_t)sent != length) {
        fprintf(stderr, "UDP send returned a partial datagram.\n");
        return false;
    }
    return true;
}

static bool register_relay(int socket_fd, const struct options *options)
{
    char hello[2049]; /* Instructor relay accepts at most 2048-byte datagrams. */
    int length;
    if (options->sending)
        length = snprintf(hello, sizeof hello, "HELLO %s send %s %s %s",
                          options->session, options->loss, options->corrupt, options->duplicate);
    else length = snprintf(hello, sizeof hello, "HELLO %s recv", options->session);
    if (length < 0 || (size_t)length >= sizeof hello) {
        fprintf(stderr, "Relay registration exceeds the 2048-byte limit.\n");
        return false;
    }
    for (unsigned attempt = 0; attempt < 5; ++attempt) {
        if (!send_bytes(socket_fd, (const uint8_t *)hello, (size_t)length)) return false;
        uint64_t now;
        if (!monotonic_ms(&now)) return false;
        uint64_t deadline = now + 1000;
        for (;;) {
            int ready = wait_until(socket_fd, deadline);
            if (ready < 0) return false;
            if (ready == 0) break;
            char reply[256];
            ssize_t count = recv(socket_fd, reply, sizeof reply - 1, 0);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) { perror("relay reply"); return false; }
            reply[(size_t)count] = '\0';
            if (count == 2 && memcmp(reply, "OK", 2) == 0) return true;
            if (count >= 4 && memcmp(reply, "ERR ", 4) == 0) {
                fprintf(stderr, "Relay refused registration: %s\n", reply + 4);
                return false;
            }
            /* Ignore stale binary traffic while awaiting the textual reply. */
        }
        fprintf(stderr, "No relay reply (attempt %u/5).\n", attempt + 1);
    }
    fprintf(stderr, "Relay registration timed out after five attempts.\n");
    return false;
}

static bool send_packet(int socket_fd, const rdt_packet *packet)
{
    uint8_t bytes[RDT_MAX_PACKET_SIZE];
    size_t length = rdt_packet_encode(packet, bytes, sizeof bytes);
    if (length == 0) {
        fprintf(stderr, "Internal packet encoding error.\n");
        return false;
    }
    return send_bytes(socket_fd, bytes, length);
}

/* An extra byte detects oversized datagrams even if recv truncates them. */
static int receive_packet(int socket_fd, rdt_packet *packet)
{
    uint8_t bytes[RDT_MAX_PACKET_SIZE + 1];
    ssize_t count = recv(socket_fd, bytes, sizeof bytes, 0);
    if (count < 0 && errno == EINTR) return 0;
    if (count < 0) { perror("recv"); return -1; }
    return rdt_packet_decode(bytes, (size_t)count, packet) ? 1 : 0;
}

static bool read_input(const char *path, uint8_t **bytes, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) { perror(path); return false; }
    bool success = false;
    *bytes = NULL;
    if (fseek(file, 0, SEEK_END) != 0) { perror("seek input"); goto cleanup; }
    long size = ftell(file);
    if (size < 0) { perror("input size"); goto cleanup; }
    if ((unsigned long)size > RDT_MAX_FILE_SIZE) {
        fprintf(stderr, "Input file exceeds the 16 MiB limit.\n");
        goto cleanup;
    }
    if (fseek(file, 0, SEEK_SET) != 0) { perror("rewind input"); goto cleanup; }
    *length = (size_t)size;
    *bytes = malloc(*length == 0 ? 1 : *length);
    if (*bytes == NULL) { perror("allocate input"); goto cleanup; }
    if (fread(*bytes, 1, *length, file) != *length || ferror(file)) {
        fprintf(stderr, "Could not read the complete input file.\n");
        goto cleanup;
    }
    if (fgetc(file) != EOF || ferror(file)) {
        fprintf(stderr, "Input file changed while being read.\n");
        goto cleanup;
    }
    success = true;
cleanup:
    if (fclose(file) != 0) { perror("close input"); success = false; }
    if (!success) { free(*bytes); *bytes = NULL; }
    return success;
}

static int run_sender(int socket_fd, const struct options *options,
                      const uint8_t *bytes, size_t length)
{
    gbn_sender sender;
    gbn_batch batch;
    if (!gbn_sender_init(&sender, bytes, length, options->window, options->timeout_ms))
        return 2;
    while (sender.status == RDT_ACTIVE) {
        uint64_t now;
        if (!monotonic_ms(&now)) return 2;
        gbn_sender_send(&sender, now, &batch);
        for (size_t i = 0; i < batch.count; ++i)
            if (!send_packet(socket_fd, &batch.packets[i])) return 2;
        int ready = wait_until(socket_fd, sender.deadline_ms);
        if (ready < 0 || !monotonic_ms(&now)) return 2;
        if (ready > 0) {
            rdt_packet ack;
            int received = receive_packet(socket_fd, &ack);
            if (received < 0) return 2;
            if (received > 0) gbn_sender_ack(&sender, &ack, now);
        }
        /* Check time even after bad or duplicate ACKs: they must not starve it. */
        if (!monotonic_ms(&now)) return 2;
        gbn_sender_timeout(&sender, now, &batch);
        for (size_t i = 0; i < batch.count; ++i)
            if (!send_packet(socket_fd, &batch.packets[i])) return 2;
    }
    if (sender.status != RDT_DONE) {
        fprintf(stderr, "Transfer gave up after 10 consecutive timeouts without progress.\n");
        return 2;
    }
    printf("Sent %zu bytes successfully.\n", length);
    return 0;
}

static int run_receiver(int socket_fd, FILE **output)
{
    uint64_t now;
    if (!monotonic_ms(&now)) return 2;
    gbn_receiver receiver;
    gbn_receiver_init(&receiver, now);
    size_t delivered = 0;
    while (receiver.status == RDT_ACTIVE) {
        uint64_t deadline = receiver.finishing ? receiver.linger_until_ms :
                            receiver.last_activity_ms + RDT_RECEIVER_IDLE_MS;
        int ready = wait_until(socket_fd, deadline);
        if (ready < 0 || !monotonic_ms(&now)) return 2;
        gbn_receiver_tick(&receiver, now);
        if (receiver.status != RDT_ACTIVE) break;
        /* A timeout reaches the idle/linger deadline, handled by tick above. */
        rdt_packet packet;
        int received = receive_packet(socket_fd, &packet);
        if (received < 0) return 2;
        if (received == 0) continue;
        gbn_receive_action action;
        gbn_receiver_receive(&receiver, &packet, now, &action);
        if (action.size > 0) {
            /* The receiver state machine enforces the file-size limit. */
            if (fwrite(action.deliver, 1, action.size, *output) != action.size) {
                perror("write output");
                return 2;
            }
            delivered += action.size;
        }
        if (action.finish) {
            int closed = fclose(*output);
            *output = NULL;
            if (closed != 0) { perror("close output"); return 2; }
        }
        if (action.ack && !send_packet(socket_fd, &action.packet)) return 2;
    }
    if (receiver.status != RDT_DONE) {
        fprintf(stderr, "Receiver timed out or exceeded the 16 MiB file limit.\n");
        return 2;
    }
    printf("Received %zu bytes successfully.\n", delivered);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 1) { usage(stdout); return 0; }
    struct options options;
    if (!parse_options(argc, argv, &options)) { usage(stderr); return 1; }
    uint8_t *bytes = NULL;
    size_t length = 0;
    FILE *output = NULL;
    int socket_fd = -1, result = 2;
    if (options.sending && !read_input(options.file, &bytes, &length)) goto cleanup;
    socket_fd = connect_relay(&options);
    if (socket_fd < 0) goto cleanup;
    if (!register_relay(socket_fd, &options)) goto cleanup;
    if (options.sending) result = run_sender(socket_fd, &options, bytes, length);
    else {
        output = fopen(options.file, "wb");
        if (output == NULL) { perror(options.file); goto cleanup; }
        result = run_receiver(socket_fd, &output);
    }
cleanup:
    if (output != NULL && fclose(output) != 0) { perror("close output"); result = 2; }
    if (socket_fd >= 0) close(socket_fd);
    free(bytes);
    return result;
}
