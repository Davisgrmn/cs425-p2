#ifndef LAB_H
#define LAB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RDT_HEADER_SIZE 10U
#define RDT_PAYLOAD_SIZE 1024U
#define RDT_MAX_PACKET_SIZE (RDT_HEADER_SIZE + RDT_PAYLOAD_SIZE)
#define RDT_MAX_FILE_SIZE (16U * 1024U * 1024U)
#define RDT_MAX_WINDOW 64U
#define RDT_MAX_TIMEOUTS 10U
#define RDT_RECEIVER_IDLE_MS UINT64_C(30000)
#define RDT_LINGER_MS UINT64_C(2000)

typedef enum {
    RDT_DATA = 0,
    RDT_ACK = 1,
    RDT_FIN = 2
} rdt_type;

typedef enum {
    RDT_ACTIVE = 0,
    RDT_DONE = 1,
    RDT_FAILED = 2
} rdt_status;

/* Host-order representation; never send this structure directly. */
typedef struct {
    uint8_t type;
    uint32_t seq;
    uint16_t length;
    uint8_t payload[RDT_PAYLOAD_SIZE];
} rdt_packet;

uint16_t rdt_checksum(const uint8_t *bytes, size_t size);
/* Encode returns the datagram size, or zero for invalid input/capacity. */
size_t rdt_packet_encode(const rdt_packet *packet, uint8_t *bytes,
                         size_t capacity);
/* Decode rejects malformed/corrupt datagrams without modifying packet. */
bool rdt_packet_decode(const uint8_t *bytes, size_t size, rdt_packet *packet);

typedef struct {
    rdt_packet packets[RDT_MAX_WINDOW];
    size_t count;
} gbn_batch;

typedef struct {
    const uint8_t *data;
    size_t size;
    uint32_t base;
    uint32_t next;
    uint32_t data_count;
    size_t window;
    uint64_t timeout_ms;
    uint64_t deadline_ms;
    unsigned consecutive_timeouts;
    rdt_status status;
    rdt_packet saved[RDT_MAX_WINDOW];
} gbn_sender;

/* Input data must remain valid and unchanged until the sender finishes. */
bool gbn_sender_init(gbn_sender *sender, const uint8_t *data, size_t size,
                     size_t window, uint64_t timeout_ms);
/* Each operation clears its output batch before adding packets to transmit. */
void gbn_sender_send(gbn_sender *sender, uint64_t now_ms, gbn_batch *batch);
void gbn_sender_ack(gbn_sender *sender, const rdt_packet *packet,
                    uint64_t now_ms);
void gbn_sender_timeout(gbn_sender *sender, uint64_t now_ms, gbn_batch *batch);

typedef struct {
    uint32_t expected;
    size_t received_size;
    uint64_t last_activity_ms;
    uint64_t linger_until_ms;
    bool finishing;
    rdt_status status;
} gbn_receiver;

typedef struct {
    bool ack;
    rdt_packet packet;
    /* A borrowed view of receive's packet payload, valid until it changes. */
    const uint8_t *deliver;
    size_t size;
    /* True once, when an in-order FIN requires the output file to close. */
    bool finish;
} gbn_receive_action;

void gbn_receiver_init(gbn_receiver *receiver, uint64_t now_ms);
void gbn_receiver_receive(gbn_receiver *receiver, const rdt_packet *packet,
                          uint64_t now_ms, gbn_receive_action *action);
rdt_status gbn_receiver_tick(gbn_receiver *receiver, uint64_t now_ms);

#endif
