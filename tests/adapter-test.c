/* Exercise the actual I/O adapter with deterministic system-call substitutes.
 * The release/coverage executables still call libc and use the real relay.
 * Including the adapter here makes its private helpers testable without adding
 * test switches or a mocking API to production code. gcov attributes it to src.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "harness/unity.h"
#include "../src/lab.h"

enum operation {
    CLOCK, POLL, RESOLVE, SOCKET, CONNECT, SEND, RECV, OPEN, SEEK, TELL,
    ALLOCATE, READ, FILE_ERROR, GET_CHAR, CLOSE_FILE, WRITE, FORMAT, OP_COUNT
};
struct fault {
    unsigned at, seen;
    long result;
    int error;
    bool repeat;
};
struct datagram { uint8_t bytes[2048]; size_t size; };
static struct {
    struct fault faults[OP_COUNT];
    uint64_t now;
    short poll_events;
    long file_size;
    bool large_transfer;
    uint32_t generated_packets;
    unsigned socket_closes, address_frees, input_closes, output_closes;
    struct datagram incoming[32], outgoing[64];
    size_t in_count, in_next, out_count, written;
    uint8_t output[4096];
} fixture;
static struct addrinfo fake_addresses[2];
static FILE input_handle, output_handle;
#define INPUT (&input_handle)
#define OUTPUT (&output_handle)

static bool injected(enum operation op, long *result)
{
    struct fault *f = &fixture.faults[op];
    ++f->seen;
    if (f->at != 0 && (f->seen == f->at || (f->repeat && f->seen >= f->at))) {
        *result = f->result;
        errno = f->error;
        return true;
    }
    return false;
}
static void fail_at(enum operation op, unsigned at, long result, int error)
{
    fixture.faults[op] = (struct fault){.at=at, .result=result, .error=error};
}
static int fake_clock_gettime(clockid_t clock_id, struct timespec *stamp)
{
    long result;
    TEST_ASSERT_EQUAL_INT(CLOCK_MONOTONIC, clock_id);
    if (injected(CLOCK, &result)) return (int)result;
    ++fixture.now;
    stamp->tv_sec = (time_t)(fixture.now / 1000);
    stamp->tv_nsec = (long)(fixture.now % 1000) * 1000000L;
    return 0;
}
static int fake_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    long result;
    TEST_ASSERT_EQUAL_UINT(1, count);
    TEST_ASSERT_EQUAL_INT(POLLIN, fds[0].events);
    TEST_ASSERT_GREATER_THAN_INT(0, timeout);
    if (injected(POLL, &result)) {
        fds[0].revents = fixture.poll_events;
        return (int)result;
    }
    if (fixture.in_next < fixture.in_count || fixture.large_transfer) {
        fds[0].revents = fixture.poll_events;
        return 1;
    }
    fixture.now += (unsigned)timeout;
    return 0;
}
static int fake_getaddrinfo(const char *host, const char *service,
                            const struct addrinfo *hints, struct addrinfo **out)
{
    long result;
    TEST_ASSERT_NOT_NULL(host);
    TEST_ASSERT_NOT_NULL(service);
    TEST_ASSERT_EQUAL_INT(AF_INET, hints->ai_family);
    TEST_ASSERT_EQUAL_INT(SOCK_DGRAM, hints->ai_socktype);
    if (injected(RESOLVE, &result)) return (int)result;
    *out = fake_addresses;
    return 0;
}
static void fake_freeaddrinfo(struct addrinfo *list)
{
    TEST_ASSERT_EQUAL_PTR(fake_addresses, list);
    ++fixture.address_frees;
}
static int fake_socket(int domain, int type, int protocol)
{
    long result;
    (void)domain; (void)type; (void)protocol;
    return injected(SOCKET, &result) ? (int)result : 10;
}
static int fake_connect(int fd, const struct sockaddr *addr, socklen_t length)
{
    long result;
    (void)fd; (void)addr; (void)length;
    return injected(CONNECT, &result) ? (int)result : 0;
}
static int fake_close(int fd)
{
    TEST_ASSERT_EQUAL_INT(10, fd);
    ++fixture.socket_closes;
    return 0;
}
static ssize_t fake_send(int fd, const void *bytes, size_t size, int flags)
{
    long result;
    (void)fd;
    TEST_ASSERT_EQUAL_INT(0, flags);
    if (injected(SEND, &result)) return (ssize_t)result;
    TEST_ASSERT_LESS_OR_EQUAL_UINT(sizeof fixture.outgoing[0].bytes, size);
    if (fixture.out_count < 64) {
        struct datagram *out = &fixture.outgoing[fixture.out_count];
        memcpy(out->bytes, bytes, size);
        out->size = size;
    }
    ++fixture.out_count;
    return (ssize_t)size;
}
static ssize_t fake_recv(int fd, void *bytes, size_t capacity, int flags)
{
    long result;
    (void)fd;
    TEST_ASSERT_EQUAL_INT(0, flags);
    if (injected(RECV, &result)) return (ssize_t)result;
    if (fixture.in_next == fixture.in_count && fixture.large_transfer) {
        rdt_packet packet = {.type=RDT_DATA, .seq=fixture.generated_packets++,
                             .length=RDT_PAYLOAD_SIZE};
        return (ssize_t)rdt_packet_encode(&packet, bytes, capacity);
    }
    TEST_ASSERT_LESS_THAN_UINT(fixture.in_count, fixture.in_next);
    struct datagram *in = &fixture.incoming[fixture.in_next++];
    size_t size = in->size < capacity ? in->size : capacity;
    memcpy(bytes, in->bytes, size);
    return (ssize_t)size;
}
static FILE *fake_fopen(const char *path, const char *mode)
{
    long result;
    TEST_ASSERT_NOT_NULL(path);
    if (injected(OPEN, &result)) return NULL;
    return strcmp(mode, "rb") == 0 ? INPUT : OUTPUT;
}
static int fake_fseek(FILE *file, long offset, int whence)
{
    long result;
    TEST_ASSERT_EQUAL_PTR(INPUT, file);
    TEST_ASSERT_EQUAL_INT(0, offset);
    TEST_ASSERT_TRUE(whence == SEEK_SET || whence == SEEK_END);
    return injected(SEEK, &result) ? (int)result : 0;
}
static long fake_ftell(FILE *file)
{
    long result;
    TEST_ASSERT_EQUAL_PTR(INPUT, file);
    return injected(TELL, &result) ? result : fixture.file_size;
}
static void *fake_malloc(size_t size)
{
    long result;
    if (injected(ALLOCATE, &result)) return NULL;
    return malloc(size);
}
static size_t fake_fread(void *bytes, size_t size, size_t count, FILE *file)
{
    long result;
    TEST_ASSERT_EQUAL_PTR(INPUT, file);
    TEST_ASSERT_EQUAL_UINT(1, size);
    if (injected(READ, &result)) return (size_t)result;
    memset(bytes, 0xa5, count);
    return count;
}
static int fake_ferror(FILE *file)
{
    long result;
    TEST_ASSERT_EQUAL_PTR(INPUT, file);
    return injected(FILE_ERROR, &result) ? (int)result : 0;
}
static int fake_fgetc(FILE *file)
{
    long result;
    TEST_ASSERT_EQUAL_PTR(INPUT, file);
    return injected(GET_CHAR, &result) ? (int)result : EOF;
}
static int fake_fclose(FILE *file)
{
    long result;
    if (file == INPUT) ++fixture.input_closes;
    else { TEST_ASSERT_EQUAL_PTR(OUTPUT, file); ++fixture.output_closes; }
    return injected(CLOSE_FILE, &result) ? (int)result : 0;
}
static size_t fake_fwrite(const void *bytes, size_t size, size_t count, FILE *file)
{
    long result;
    TEST_ASSERT_EQUAL_PTR(OUTPUT, file);
    TEST_ASSERT_EQUAL_UINT(1, size);
    if (injected(WRITE, &result)) return (size_t)result;
    if (fixture.written + count <= sizeof fixture.output)
        memcpy(fixture.output + fixture.written, bytes, count);
    fixture.written += count;
    return count;
}
static int fake_snprintf(char *buffer, size_t size, const char *format, ...)
{
    long result;
    if (injected(FORMAT, &result)) return (int)result;
    va_list args;
    va_start(args, format);
    int count = vsnprintf(buffer, size, format, args);
    va_end(args);
    return count;
}

#define clock_gettime fake_clock_gettime
#define poll fake_poll
#define getaddrinfo fake_getaddrinfo
#define freeaddrinfo fake_freeaddrinfo
#define socket fake_socket
#define connect fake_connect
#define close fake_close
#define send fake_send
#define recv fake_recv
#define fopen fake_fopen
#define fseek fake_fseek
#define ftell fake_ftell
#define malloc fake_malloc
#define fread fake_fread
#define ferror fake_ferror
#define fgetc fake_fgetc
#define fclose fake_fclose
#define fwrite fake_fwrite
#define snprintf fake_snprintf
#define main application_main
#include "../src/main.c"
#undef main
#undef clock_gettime
#undef poll
#undef getaddrinfo
#undef freeaddrinfo
#undef socket
#undef connect
#undef close
#undef send
#undef recv
#undef fopen
#undef fseek
#undef ftell
#undef malloc
#undef fread
#undef ferror
#undef fgetc
#undef fclose
#undef fwrite
#undef snprintf

void setUp(void)
{
    memset(&fixture, 0, sizeof fixture);
    fixture.now = 100;
    fixture.file_size = 3;
    fixture.poll_events = POLLIN;
    memset(fake_addresses, 0, sizeof fake_addresses);
    fake_addresses[0].ai_next = &fake_addresses[1];
    optind = 0; /* Fully reset GNU getopt before each independent invocation. */
}
void tearDown(void) {}
static void queue_bytes(const void *bytes, size_t size)
{
    TEST_ASSERT_LESS_THAN_UINT(32, fixture.in_count);
    struct datagram *in = &fixture.incoming[fixture.in_count++];
    TEST_ASSERT_LESS_OR_EQUAL_UINT(sizeof in->bytes, size);
    memcpy(in->bytes, bytes, size);
    in->size = size;
}
static void queue_packet(uint8_t type, uint32_t seq, const char *payload)
{
    rdt_packet packet = {.type=type, .seq=seq};
    if (payload != NULL) {
        packet.length = (uint16_t)strlen(payload);
        memcpy(packet.payload, payload, packet.length);
    }
    uint8_t bytes[RDT_MAX_PACKET_SIZE];
    size_t size = rdt_packet_encode(&packet, bytes, sizeof bytes);
    TEST_ASSERT_GREATER_THAN_UINT(0, size);
    queue_bytes(bytes, size);
}
static struct options options(bool sender)
{
    return (struct options){.sending=sender, .session="test-1", .host="localhost",
        .file="file.bin", .window=2, .timeout_ms=250, .port=4250,
        .loss="0", .corrupt="0", .duplicate="0"};
}
static void test_decimal_and_integer_validation(void)
{
    unsigned value;
    const char *rate;
    TEST_ASSERT_TRUE(integer_option("65535", 65535, &value));
    TEST_ASSERT_EQUAL_UINT(65535, value);
    const char *bad_int[] = {"", "-1", "x", "0", "2x", "65536", "999999999999999999999999"};
    for (size_t i=0; i<sizeof bad_int/sizeof *bad_int; ++i)
        TEST_ASSERT_FALSE(integer_option(bad_int[i], 65535, &value));
    const char *good[] = {"0", ".5", "0.00001", "00.050"};
    for (size_t i=0; i<sizeof good/sizeof *good; ++i) {
        TEST_ASSERT_TRUE(probability_option(good[i], &rate));
        TEST_ASSERT_EQUAL_PTR(good[i], rate);
    }
    const char *bad[] = {"", ".", "0.", "x", ".5x", "-0.1", "1e-5", "0.51", ".x"};
    for (size_t i=0; i<sizeof bad/sizeof *bad; ++i)
        TEST_ASSERT_FALSE(probability_option(bad[i], &rate));
    char huge[401]; memset(huge, '9', 400); huge[400]='\0';
    TEST_ASSERT_FALSE(probability_option(huge, &rate));
    TEST_ASSERT_FALSE(session_valid(NULL));
    TEST_ASSERT_FALSE(session_valid(""));
    TEST_ASSERT_FALSE(session_valid("UPPER"));
    TEST_ASSERT_FALSE(session_valid("abc_"));
    TEST_ASSERT_FALSE(session_valid("{"));
    TEST_ASSERT_FALSE(session_valid("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    TEST_ASSERT_TRUE(session_valid("az09-"));
}
static void test_cli_modes_and_options(void)
{
    struct options parsed;
    char *good[] = {"myapp", "send", "-s", "test-1", "-w", "64", "-T", "1",
        "-p", "65535", "-l", ".1", "-c", ".2", "-d", ".3", "localhost", "input", NULL};
    TEST_ASSERT_TRUE(parse_options(18, good, &parsed));
    TEST_ASSERT_EQUAL_UINT(64, parsed.window);
    TEST_ASSERT_EQUAL_UINT(1, parsed.timeout_ms);
    TEST_ASSERT_EQUAL_UINT(65535, parsed.port);
    TEST_ASSERT_EQUAL_STRING(".3", parsed.duplicate);
    const char *bad_flags[] = {"-w", "-T", "-p", "-l", "-c", "-d", "-x"};
    for (size_t i=0;i<sizeof bad_flags/sizeof *bad_flags;++i) {
        char *args[] = {"myapp", "send", "-s", "ok", (char *)bad_flags[i], "bad", "host", "file", NULL};
        optind=0;
        TEST_ASSERT_FALSE(parse_options(8, args, &parsed));
    }
    char *missing[] = {"myapp", "send", "host", "file", NULL};
    optind=0; TEST_ASSERT_FALSE(parse_options(4, missing, &parsed));
    char *invalid[] = {"myapp", "other", NULL};
    TEST_ASSERT_FALSE(parse_options(2, invalid, &parsed));
    char *short_args[] = {"myapp", "send", NULL};
    optind=0; TEST_ASSERT_FALSE(parse_options(2, short_args, &parsed));
    char *recv_args[] = {"myapp", "recv", "-s", "ok", "-w", "1", "host", "file", NULL};
    optind=0; TEST_ASSERT_FALSE(parse_options(8, recv_args, &parsed));
    char *empty[] = {"myapp", "recv", "-s", "ok", "", "file", NULL};
    optind=0; TEST_ASSERT_FALSE(parse_options(6, empty, &parsed));
    empty[4]="host"; empty[5]="";
    optind=0; TEST_ASSERT_FALSE(parse_options(6, empty, &parsed));
    empty[5]="file";
    optind=0; TEST_ASSERT_TRUE(parse_options(6, empty, &parsed));
    char *recv_port[]={"myapp","recv","-s","ok","-p","24500","host","file",NULL};
    optind=0; TEST_ASSERT_TRUE(parse_options(8,recv_port,&parsed));
    TEST_ASSERT_EQUAL_UINT(24500,parsed.port);
}
static void test_clock_and_poll_outcomes(void)
{
    uint64_t now;
    TEST_ASSERT_TRUE(monotonic_ms(&now)); TEST_ASSERT_EQUAL_UINT64(101, now);
    fail_at(CLOCK, 1, -1, EIO); TEST_ASSERT_FALSE(monotonic_ms(&now));
    setUp(); TEST_ASSERT_EQUAL_INT(0, wait_until(10, fixture.now));
    setUp(); TEST_ASSERT_EQUAL_INT(0, wait_until(10, (uint64_t)INT_MAX+200));
    setUp(); fail_at(CLOCK, 1, -1, EIO); TEST_ASSERT_EQUAL_INT(-1, wait_until(10, 200));
    setUp(); fail_at(POLL, 1, -1, EINTR); TEST_ASSERT_EQUAL_INT(0, wait_until(10, 200));
    setUp(); fail_at(POLL, 1, -1, EIO); TEST_ASSERT_EQUAL_INT(-1, wait_until(10, 200));
    setUp(); queue_bytes("OK", 2); TEST_ASSERT_EQUAL_INT(1, wait_until(10, 200));
    setUp(); fixture.poll_events=POLLERR; queue_bytes("OK", 2);
    TEST_ASSERT_EQUAL_INT(-1, wait_until(10, 200));
}
static void test_resolution_and_socket_fallback(void)
{
    struct options opts=options(true);
    TEST_ASSERT_EQUAL_INT(10, connect_relay(&opts));
    TEST_ASSERT_EQUAL_UINT(1, fixture.address_frees);
    setUp(); fail_at(RESOLVE, 1, EAI_NONAME, 0);
    TEST_ASSERT_EQUAL_INT(-1, connect_relay(&opts)); TEST_ASSERT_EQUAL_UINT(0, fixture.address_frees);
    setUp(); fail_at(SOCKET, 1, -1, EMFILE);
    TEST_ASSERT_EQUAL_INT(10, connect_relay(&opts)); TEST_ASSERT_EQUAL_UINT(2, fixture.faults[SOCKET].seen);
    setUp(); fail_at(CONNECT, 1, -1, EHOSTUNREACH);
    TEST_ASSERT_EQUAL_INT(10, connect_relay(&opts)); TEST_ASSERT_EQUAL_UINT(1, fixture.socket_closes);
    setUp(); fail_at(SOCKET, 1, -1, EMFILE); fixture.faults[SOCKET].repeat=true;
    TEST_ASSERT_EQUAL_INT(-1, connect_relay(&opts)); TEST_ASSERT_EQUAL_UINT(1, fixture.address_frees);
    setUp(); fail_at(CONNECT, 1, -1, EHOSTUNREACH); fixture.faults[CONNECT].repeat=true;
    TEST_ASSERT_EQUAL_INT(-1, connect_relay(&opts)); TEST_ASSERT_EQUAL_UINT(2, fixture.socket_closes);
}
static void test_datagram_send_and_receive_failures(void)
{
    const uint8_t bytes[] = {0,1,2};
    TEST_ASSERT_TRUE(send_bytes(10, bytes, sizeof bytes));
    setUp(); fail_at(SEND, 1, -1, EINTR);
    TEST_ASSERT_TRUE(send_bytes(10, bytes, sizeof bytes)); TEST_ASSERT_EQUAL_UINT(2, fixture.faults[SEND].seen);
    setUp(); fail_at(SEND, 1, -1, EIO); TEST_ASSERT_FALSE(send_bytes(10, bytes, sizeof bytes));
    setUp(); fail_at(SEND, 1, 1, 0); TEST_ASSERT_FALSE(send_bytes(10, bytes, sizeof bytes));
    setUp(); rdt_packet packet={.type=99}; TEST_ASSERT_FALSE(send_packet(10, &packet));
    packet.type=RDT_FIN; TEST_ASSERT_TRUE(send_packet(10, &packet));
    setUp(); fail_at(RECV, 1, -1, EINTR); TEST_ASSERT_EQUAL_INT(0, receive_packet(10, &packet));
    setUp(); fail_at(RECV, 1, -1, EIO); TEST_ASSERT_EQUAL_INT(-1, receive_packet(10, &packet));
    setUp(); queue_bytes("bad",3); TEST_ASSERT_EQUAL_INT(0, receive_packet(10, &packet));
    queue_packet(RDT_ACK, 3, NULL); TEST_ASSERT_EQUAL_INT(1, receive_packet(10, &packet));
    TEST_ASSERT_EQUAL_UINT(3, packet.seq);
}
static void test_registration_replies_and_retries(void)
{
    struct options opts=options(true);
    opts.loss="0.00001";
    queue_bytes("NO",2); queue_bytes("ERR",3); queue_bytes("stale",5); queue_bytes("OK",2);
    TEST_ASSERT_TRUE(register_relay(10, &opts));
    const char expected[]="HELLO test-1 send 0.00001 0 0";
    TEST_ASSERT_EQUAL_UINT(strlen(expected), fixture.outgoing[0].size);
    TEST_ASSERT_EQUAL_MEMORY(expected, fixture.outgoing[0].bytes, strlen(expected));
    setUp(); opts.sending=false; queue_bytes("OK",2);
    TEST_ASSERT_TRUE(register_relay(10, &opts));
    TEST_ASSERT_EQUAL_MEMORY("HELLO test-1 recv", fixture.outgoing[0].bytes, 17);
    setUp(); queue_bytes("ERR no receiver",15); TEST_ASSERT_FALSE(register_relay(10, &opts));
    setUp(); TEST_ASSERT_FALSE(register_relay(10, &opts)); TEST_ASSERT_EQUAL_UINT(5, fixture.out_count);
    setUp(); queue_bytes("OK",2); fail_at(RECV,1,-1,EINTR);
    TEST_ASSERT_TRUE(register_relay(10, &opts)); TEST_ASSERT_EQUAL_UINT(2, fixture.faults[RECV].seen);
    setUp(); queue_bytes("OK",2); fail_at(RECV,1,-1,EIO);
    TEST_ASSERT_FALSE(register_relay(10, &opts));
    setUp(); fail_at(SEND,1,-1,EIO); TEST_ASSERT_FALSE(register_relay(10, &opts));
    setUp(); fail_at(CLOCK,1,-1,EIO); TEST_ASSERT_FALSE(register_relay(10, &opts));
    setUp(); fail_at(POLL,1,-1,EIO); TEST_ASSERT_FALSE(register_relay(10, &opts));
    setUp(); fail_at(FORMAT,1,-1,EILSEQ); TEST_ASSERT_FALSE(register_relay(10, &opts));
    setUp(); char huge[2049]; memset(huge,'0',2048); huge[2048]='\0';
    opts.sending=true; opts.loss=huge; TEST_ASSERT_FALSE(register_relay(10, &opts));
}
static void test_input_success_and_file_failures(void)
{
    uint8_t *bytes=NULL; size_t length=0;
    TEST_ASSERT_TRUE(read_input("input", &bytes, &length));
    TEST_ASSERT_EQUAL_UINT(3,length); TEST_ASSERT_EQUAL_HEX8(0xa5,bytes[2]);
    free(bytes); TEST_ASSERT_EQUAL_UINT(1,fixture.input_closes);
    setUp(); fixture.file_size=0;
    TEST_ASSERT_TRUE(read_input("input",&bytes,&length)); TEST_ASSERT_EQUAL_UINT(0,length); free(bytes);
    const struct {enum operation op; unsigned at; long result;} failures[] = {
        {OPEN,1,0}, {SEEK,1,-1}, {TELL,1,-1}, {SEEK,2,-1}, {ALLOCATE,1,0},
        {READ,1,0}, {FILE_ERROR,1,1}, {GET_CHAR,1,0}, {FILE_ERROR,2,1}, {CLOSE_FILE,1,EOF}
    };
    for(size_t i=0;i<sizeof failures/sizeof *failures;++i) {
        setUp(); bytes=NULL;
        fail_at(failures[i].op,failures[i].at,failures[i].result,EIO);
        TEST_ASSERT_FALSE(read_input("input",&bytes,&length));
        TEST_ASSERT_NULL(bytes);
        TEST_ASSERT_EQUAL_UINT(failures[i].op==OPEN?0:1, fixture.input_closes);
    }
    setUp(); fixture.file_size=(long)RDT_MAX_FILE_SIZE+1;
    TEST_ASSERT_FALSE(read_input("input",&bytes,&length));
    TEST_ASSERT_NULL(bytes); TEST_ASSERT_EQUAL_UINT(1,fixture.input_closes);
}
static void sender_replies(void)
{
    queue_packet(RDT_ACK,1,NULL); queue_packet(RDT_ACK,2,NULL);
}
static void test_sender_loop_success_and_bad_acks(void)
{
    struct options opts=options(true);
    queue_bytes("bad",3); sender_replies();
    TEST_ASSERT_EQUAL_INT(0,run_sender(10,&opts,(const uint8_t *)"abc",3));
    TEST_ASSERT_EQUAL_UINT(2,fixture.out_count);
    rdt_packet data;
    TEST_ASSERT_TRUE(rdt_packet_decode(fixture.outgoing[0].bytes,fixture.outgoing[0].size,&data));
    TEST_ASSERT_EQUAL_MEMORY("abc",data.payload,3);
    TEST_ASSERT_TRUE(rdt_packet_decode(fixture.outgoing[1].bytes,fixture.outgoing[1].size,&data));
    TEST_ASSERT_EQUAL_INT(RDT_FIN,data.type);
    setUp(); opts.window=0;
    TEST_ASSERT_EQUAL_INT(2,run_sender(10,&opts,(const uint8_t *)"abc",3));
}
static void test_sender_loop_errors_and_timeout_cleanup(void)
{
    struct options opts=options(true);
    sender_replies(); TEST_ASSERT_EQUAL_INT(0,run_sender(10,&opts,(const uint8_t *)"abc",3));
    unsigned clocks=fixture.faults[CLOCK].seen;
    for(unsigned i=1;i<=clocks;++i) {
        setUp(); sender_replies(); fail_at(CLOCK,i,-1,EIO);
        TEST_ASSERT_EQUAL_INT(2,run_sender(10,&opts,(const uint8_t *)"abc",3));
    }
    setUp(); fail_at(SEND,1,-1,EIO);
    TEST_ASSERT_EQUAL_INT(2,run_sender(10,&opts,(const uint8_t *)"abc",3));
    setUp(); fail_at(SEND,2,-1,EIO); /* Retransmission after a lost ACK. */
    TEST_ASSERT_EQUAL_INT(2,run_sender(10,&opts,(const uint8_t *)"abc",3));
    setUp(); fail_at(POLL,1,-1,EIO);
    TEST_ASSERT_EQUAL_INT(2,run_sender(10,&opts,(const uint8_t *)"abc",3));
    setUp(); sender_replies(); fail_at(RECV,1,-1,EIO);
    TEST_ASSERT_EQUAL_INT(2,run_sender(10,&opts,(const uint8_t *)"abc",3));
    setUp(); TEST_ASSERT_EQUAL_INT(2,run_sender(10,&opts,(const uint8_t *)"abc",3));
    TEST_ASSERT_EQUAL_UINT(10,fixture.out_count);
}
static void receiver_packets(void)
{
    queue_bytes("bad",3); queue_packet(RDT_ACK,9,NULL);
    queue_packet(RDT_DATA,0,"abc"); queue_packet(RDT_FIN,1,NULL);
}
static void test_receiver_loop_delivery_and_errors(void)
{
    FILE *output=OUTPUT;
    receiver_packets(); TEST_ASSERT_EQUAL_INT(0,run_receiver(10,&output));
    TEST_ASSERT_NULL(output); TEST_ASSERT_EQUAL_UINT(1,fixture.output_closes);
    TEST_ASSERT_EQUAL_UINT(3,fixture.written); TEST_ASSERT_EQUAL_MEMORY("abc",fixture.output,3);
    unsigned clocks=fixture.faults[CLOCK].seen;
    for(unsigned i=1;i<=clocks;++i) {
        setUp(); output=OUTPUT; receiver_packets(); fail_at(CLOCK,i,-1,EIO);
        TEST_ASSERT_EQUAL_INT(2,run_receiver(10,&output));
    }
    const enum operation errors[]={POLL,RECV,WRITE,CLOSE_FILE,SEND};
    for(size_t i=0;i<sizeof errors/sizeof *errors;++i) {
        setUp(); output=OUTPUT; receiver_packets(); fail_at(errors[i],1,errors[i]==WRITE?0:-1,EIO);
        TEST_ASSERT_EQUAL_INT(2,run_receiver(10,&output));
        if(errors[i]==CLOSE_FILE) TEST_ASSERT_NULL(output);
    }
    setUp(); output=OUTPUT;
    TEST_ASSERT_EQUAL_INT(2,run_receiver(10,&output)); /* Idle timeout. */
}
static void test_receiver_rejects_data_above_file_limit(void)
{
    FILE *output=OUTPUT;
    fixture.large_transfer=true;
    TEST_ASSERT_EQUAL_INT(2,run_receiver(10,&output));
    TEST_ASSERT_EQUAL_UINT(RDT_MAX_FILE_SIZE,fixture.written);
    TEST_ASSERT_EQUAL_UINT(RDT_MAX_FILE_SIZE/RDT_PAYLOAD_SIZE+1,fixture.generated_packets);
    TEST_ASSERT_EQUAL_UINT(RDT_MAX_FILE_SIZE/RDT_PAYLOAD_SIZE,fixture.out_count);
    TEST_ASSERT_EQUAL_PTR(OUTPUT,output); /* Caller must still close on failure. */
}
static int invoke_main(bool sender)
{
    char *args[]={"myapp",sender?"send":"recv","-s","test-1","localhost","file",NULL};
    optind=0;
    return application_main(6,args);
}
static void test_application_exit_codes_and_resource_cleanup(void)
{
    char *usage_args[]={"myapp",NULL};
    TEST_ASSERT_EQUAL_INT(0,application_main(1,usage_args));
    char *bad_args[]={"myapp","invalid",NULL};
    TEST_ASSERT_EQUAL_INT(1,application_main(2,bad_args));
    setUp(); queue_bytes("OK",2); sender_replies();
    TEST_ASSERT_EQUAL_INT(0,invoke_main(true));
    TEST_ASSERT_EQUAL_UINT(1,fixture.socket_closes); TEST_ASSERT_EQUAL_UINT(1,fixture.input_closes);
    setUp(); queue_bytes("OK",2); receiver_packets();
    TEST_ASSERT_EQUAL_INT(0,invoke_main(false));
    TEST_ASSERT_EQUAL_UINT(1,fixture.socket_closes); TEST_ASSERT_EQUAL_UINT(1,fixture.output_closes);
    setUp(); fail_at(OPEN,1,0,ENOENT); TEST_ASSERT_EQUAL_INT(2,invoke_main(true));
    TEST_ASSERT_EQUAL_UINT(0,fixture.socket_closes);
    setUp(); fail_at(RESOLVE,1,EAI_NONAME,0); TEST_ASSERT_EQUAL_INT(2,invoke_main(true));
    TEST_ASSERT_EQUAL_UINT(1,fixture.input_closes); TEST_ASSERT_EQUAL_UINT(0,fixture.socket_closes);
    setUp(); queue_bytes("ERR refused",11); TEST_ASSERT_EQUAL_INT(2,invoke_main(true));
    TEST_ASSERT_EQUAL_UINT(1,fixture.socket_closes);
    setUp(); queue_bytes("OK",2); fail_at(OPEN,1,0,EACCES);
    TEST_ASSERT_EQUAL_INT(2,invoke_main(false)); TEST_ASSERT_EQUAL_UINT(1,fixture.socket_closes);
    setUp(); queue_bytes("OK",2); fail_at(POLL,2,-1,EIO);
    TEST_ASSERT_EQUAL_INT(2,invoke_main(false)); TEST_ASSERT_EQUAL_UINT(1,fixture.output_closes);
    setUp(); queue_bytes("OK",2); fail_at(POLL,2,-1,EIO); fail_at(CLOSE_FILE,1,EOF,EIO);
    TEST_ASSERT_EQUAL_INT(2,invoke_main(false)); TEST_ASSERT_EQUAL_UINT(1,fixture.output_closes);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_decimal_and_integer_validation);
    RUN_TEST(test_cli_modes_and_options);
    RUN_TEST(test_clock_and_poll_outcomes);
    RUN_TEST(test_resolution_and_socket_fallback);
    RUN_TEST(test_datagram_send_and_receive_failures);
    RUN_TEST(test_registration_replies_and_retries);
    RUN_TEST(test_input_success_and_file_failures);
    RUN_TEST(test_sender_loop_success_and_bad_acks);
    RUN_TEST(test_sender_loop_errors_and_timeout_cleanup);
    RUN_TEST(test_receiver_loop_delivery_and_errors);
    RUN_TEST(test_receiver_rejects_data_above_file_limit);
    RUN_TEST(test_application_exit_codes_and_resource_cleanup);
    return UNITY_END();
}
