#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "harness/unity.h"
#include "../src/lab.h"

/* All protocol time is injected; these tests use neither sockets nor sleeps. */
void setUp(void) {}
void tearDown(void) {}

static rdt_packet packet(uint8_t type, uint32_t seq, const void *data, uint16_t size)
{
    rdt_packet result;
    memset(&result, 0, sizeof(result));
    result.type = type;
    result.seq = seq;
    result.length = size;
    if (size != 0 && data != NULL && size <= RDT_PAYLOAD_SIZE) {
        memcpy(result.payload, data, size);
    }
    return result;
}

static void checksum_wire(uint8_t *wire, size_t size)
{
    uint16_t checksum;
    wire[2] = 0;
    wire[3] = 0;
    checksum = rdt_checksum(wire, size);
    wire[2] = (uint8_t)(checksum >> 8);
    wire[3] = (uint8_t)checksum;
}

static void assert_packet(const rdt_packet *actual, uint8_t type,
                          uint32_t seq, const void *payload, uint16_t size)
{
    TEST_ASSERT_EQUAL_UINT8(type, actual->type);
    TEST_ASSERT_EQUAL_UINT32(seq, actual->seq);
    TEST_ASSERT_EQUAL_UINT16(size, actual->length);
    if (size != 0) {
        TEST_ASSERT_EQUAL_MEMORY(payload, actual->payload, size);
    }
}

static void acknowledge(gbn_sender *sender, uint32_t next_expected, uint64_t now)
{
    rdt_packet ack = packet(RDT_ACK, next_expected, NULL, 0);
    gbn_sender_ack(sender, &ack, now);
}

static void test_checksum_rfc1071_and_odd_lengths(void)
{
    const uint8_t rfc1071[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    const uint8_t odd[] = {0x01, 0x02, 0x03};
    const uint8_t carry[] = {0xff, 0xff, 0xff, 0xff};
    const uint8_t single[] = {0x80};
    TEST_ASSERT_EQUAL_HEX16(0x220d, rdt_checksum(rfc1071, sizeof(rfc1071)));
    TEST_ASSERT_EQUAL_HEX16(0xfbfd, rdt_checksum(odd, sizeof(odd)));
    TEST_ASSERT_EQUAL_HEX16(0x0000, rdt_checksum(carry, sizeof(carry)));
    TEST_ASSERT_EQUAL_HEX16(0x7fff, rdt_checksum(single, sizeof(single)));
    TEST_ASSERT_EQUAL_HEX16(0xffff, rdt_checksum(NULL, 0));
    TEST_ASSERT_EQUAL_HEX16(0, rdt_checksum(NULL, 1));
}

static void test_exact_wire_fixtures_and_network_order(void)
{
    const uint8_t data_fixture[] = {0x00, 0x00, 0x96, 0x91, 0x00, 0x00,
                                   0x00, 0x02, 0x00, 0x03, 0x48, 0x69, 0x21};
    const uint8_t ack_fixture[] = {0x01, 0x00, 0xfe, 0xfc, 0x00,
                                  0x00, 0x00, 0x03, 0x00, 0x00};
    uint8_t wire[RDT_MAX_PACKET_SIZE];
    rdt_packet data = packet(RDT_DATA, 2, "Hi!", 3);
    rdt_packet ack = packet(RDT_ACK, 3, NULL, 0);
    rdt_packet decoded;
    size_t size = rdt_packet_encode(&data, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_UINT(sizeof(data_fixture), size);
    TEST_ASSERT_EQUAL_MEMORY(data_fixture, wire, size);
    TEST_ASSERT_EQUAL_HEX16(0, rdt_checksum(wire, size));
    TEST_ASSERT_TRUE(rdt_packet_decode(wire, size, &decoded));
    assert_packet(&decoded, RDT_DATA, 2, "Hi!", 3);
    size = rdt_packet_encode(&ack, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_UINT(sizeof(ack_fixture), size);
    TEST_ASSERT_EQUAL_MEMORY(ack_fixture, wire, size);
    TEST_ASSERT_TRUE(rdt_packet_decode(ack_fixture, sizeof(ack_fixture), &decoded));
    assert_packet(&decoded, RDT_ACK, 3, NULL, 0);
    data = packet(RDT_DATA, UINT32_C(0x12345678), "Z", 1);
    TEST_ASSERT_EQUAL_UINT(11, rdt_packet_encode(&data, wire, sizeof(wire)));
    TEST_ASSERT_EQUAL_HEX8(0x12, wire[4]);
    TEST_ASSERT_EQUAL_HEX8(0x34, wire[5]);
    TEST_ASSERT_EQUAL_HEX8(0x56, wire[6]);
    TEST_ASSERT_EQUAL_HEX8(0x78, wire[7]);
    TEST_ASSERT_TRUE(rdt_packet_decode(wire, 11, &decoded));
    assert_packet(&decoded, RDT_DATA, UINT32_C(0x12345678), "Z", 1);
}

static void test_packet_roundtrips_boundary_payloads(void)
{
    uint8_t wire[RDT_MAX_PACKET_SIZE];
    rdt_packet original, decoded;
    original = packet(RDT_DATA, UINT32_MAX, NULL, RDT_PAYLOAD_SIZE);
    for (size_t i = 0; i < RDT_PAYLOAD_SIZE; ++i) {
        original.payload[i] = (uint8_t)(i * 37U);
    }
    TEST_ASSERT_EQUAL_UINT(RDT_MAX_PACKET_SIZE, rdt_packet_encode(&original, wire, sizeof(wire)));
    TEST_ASSERT_TRUE(rdt_packet_decode(wire, sizeof(wire), &decoded));
    assert_packet(&decoded, RDT_DATA, UINT32_MAX, original.payload, RDT_PAYLOAD_SIZE);
    for (uint8_t type = RDT_DATA; type <= RDT_FIN; ++type) {
        original = packet(type, 0, NULL, 0);
        TEST_ASSERT_EQUAL_UINT(RDT_HEADER_SIZE, rdt_packet_encode(&original, wire, sizeof(wire)));
        TEST_ASSERT_TRUE(rdt_packet_decode(wire, RDT_HEADER_SIZE, &decoded));
        assert_packet(&decoded, type, 0, NULL, 0);
    }
}

static void test_packet_encoder_rejects_invalid_arguments(void)
{
    uint8_t wire[RDT_MAX_PACKET_SIZE];
    rdt_packet p = packet(RDT_DATA, 0, "x", 1);
    TEST_ASSERT_EQUAL_UINT(0, rdt_packet_encode(NULL, wire, sizeof(wire)));
    TEST_ASSERT_EQUAL_UINT(0, rdt_packet_encode(&p, NULL, sizeof(wire)));
    TEST_ASSERT_EQUAL_UINT(0, rdt_packet_encode(&p, wire, RDT_HEADER_SIZE));
    p.type = 3;
    TEST_ASSERT_EQUAL_UINT(0, rdt_packet_encode(&p, wire, sizeof(wire)));
    p.type = RDT_DATA;
    p.length = RDT_PAYLOAD_SIZE + 1;
    TEST_ASSERT_EQUAL_UINT(0, rdt_packet_encode(&p, wire, sizeof(wire)));
    p.length = 1;
    p.type = RDT_ACK;
    TEST_ASSERT_EQUAL_UINT(0, rdt_packet_encode(&p, wire, sizeof(wire)));
    p.type = RDT_FIN;
    TEST_ASSERT_EQUAL_UINT(0, rdt_packet_encode(&p, wire, sizeof(wire)));
}

static void test_packet_decoder_rejects_each_malformed_wire_condition(void)
{
    uint8_t wire[RDT_MAX_PACKET_SIZE + 1];
    uint8_t valid[RDT_HEADER_SIZE + 3];
    rdt_packet p = packet(RDT_DATA, 2, "Hi!", 3), decoded;
    size_t size = rdt_packet_encode(&p, valid, sizeof(valid));
    TEST_ASSERT_FALSE(rdt_packet_decode(NULL, size, &decoded));
    TEST_ASSERT_FALSE(rdt_packet_decode(valid, size, NULL));
    for (size_t truncated = 0; truncated < RDT_HEADER_SIZE; ++truncated) {
        TEST_ASSERT_FALSE(rdt_packet_decode(valid, truncated, &decoded));
    }
    TEST_ASSERT_FALSE(rdt_packet_decode(valid, size - 1, &decoded));
    memcpy(wire, valid, size);
    wire[size] = 0;
    TEST_ASSERT_FALSE(rdt_packet_decode(wire, size + 1, &decoded));
    memcpy(wire, valid, size);
    wire[0] = 3;
    checksum_wire(wire, size);
    TEST_ASSERT_FALSE(rdt_packet_decode(wire, size, &decoded));
    memcpy(wire, valid, size);
    wire[1] = 1;
    checksum_wire(wire, size);
    TEST_ASSERT_FALSE(rdt_packet_decode(wire, size, &decoded));
    memcpy(wire, valid, size);
    wire[8] = 4;
    wire[9] = 1;
    checksum_wire(wire, size);
    TEST_ASSERT_FALSE(rdt_packet_decode(wire, size, &decoded));
    for (uint8_t type = RDT_ACK; type <= RDT_FIN; ++type) {
        memcpy(wire, valid, size);
        wire[0] = type;
        checksum_wire(wire, size);
        TEST_ASSERT_FALSE(rdt_packet_decode(wire, size, &decoded));
    }
    for (size_t index = 0; index < size; ++index) {
        memcpy(wire, valid, size);
        wire[index] ^= 1;
        TEST_ASSERT_FALSE(rdt_packet_decode(wire, size, &decoded));
    }
    memset(wire, 0, sizeof(wire));
    wire[8] = 4;
    wire[9] = 1;
    checksum_wire(wire, sizeof(wire));
    TEST_ASSERT_FALSE(rdt_packet_decode(wire, sizeof(wire), &decoded));
}

static void test_sender_initialization_limits(void)
{
    uint8_t byte = 0;
    gbn_sender sender;
    TEST_ASSERT_FALSE(gbn_sender_init(NULL, &byte, 1, 1, 100));
    TEST_ASSERT_FALSE(gbn_sender_init(&sender, NULL, 1, 1, 100));
    TEST_ASSERT_FALSE(gbn_sender_init(&sender, &byte, RDT_MAX_FILE_SIZE + 1U, 1, 100));
    TEST_ASSERT_FALSE(gbn_sender_init(&sender, &byte, 1, 0, 100));
    TEST_ASSERT_FALSE(gbn_sender_init(&sender, &byte, 1, RDT_MAX_WINDOW + 1U, 100));
    TEST_ASSERT_FALSE(gbn_sender_init(&sender, &byte, 1, 1, 0));
    TEST_ASSERT_TRUE(gbn_sender_init(&sender, NULL, 0, 1, 1));
    TEST_ASSERT_EQUAL_UINT(0, sender.data_count);
    TEST_ASSERT_EQUAL_INT(RDT_ACTIVE, sender.status);
    TEST_ASSERT_TRUE(gbn_sender_init(&sender, &byte, RDT_MAX_FILE_SIZE, RDT_MAX_WINDOW, 100));
    TEST_ASSERT_EQUAL_UINT32(RDT_MAX_FILE_SIZE / RDT_PAYLOAD_SIZE, sender.data_count);
}

static void test_sender_window_slides_with_cumulative_ack(void)
{
    uint8_t bytes[4 * RDT_PAYLOAD_SIZE + 7];
    gbn_sender sender;
    gbn_batch batch;
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        bytes[i] = (uint8_t)i;
    }
    TEST_ASSERT_TRUE(gbn_sender_init(&sender, bytes, sizeof(bytes), 3, 100));
    TEST_ASSERT_EQUAL_UINT32(5, sender.data_count);
    gbn_sender_send(&sender, 10, &batch);
    TEST_ASSERT_EQUAL_UINT(3, batch.count);
    for (uint32_t i = 0; i < 3; ++i) {
        assert_packet(&batch.packets[i], RDT_DATA, i, bytes + i * RDT_PAYLOAD_SIZE, RDT_PAYLOAD_SIZE);
    }
    TEST_ASSERT_EQUAL_UINT32(0, sender.base);
    TEST_ASSERT_EQUAL_UINT32(3, sender.next);
    TEST_ASSERT_EQUAL_UINT64(110, sender.deadline_ms);
    gbn_sender_send(&sender, 20, &batch);
    TEST_ASSERT_EQUAL_UINT(0, batch.count);
    TEST_ASSERT_EQUAL_UINT64(110, sender.deadline_ms);
    acknowledge(&sender, 2, 40);
    TEST_ASSERT_EQUAL_UINT32(2, sender.base);
    TEST_ASSERT_EQUAL_UINT64(140, sender.deadline_ms);
    gbn_sender_send(&sender, 41, &batch);
    TEST_ASSERT_EQUAL_UINT(2, batch.count);
    assert_packet(&batch.packets[0], RDT_DATA, 3, bytes + 3 * RDT_PAYLOAD_SIZE, RDT_PAYLOAD_SIZE);
    assert_packet(&batch.packets[1], RDT_DATA, 4, bytes + 4 * RDT_PAYLOAD_SIZE, 7);
    TEST_ASSERT_EQUAL_UINT64(140, sender.deadline_ms);
    acknowledge(&sender, 4, 60);
    gbn_sender_send(&sender, 61, &batch);
    TEST_ASSERT_EQUAL_UINT(0, batch.count); /* FIN waits for the last DATA ACK. */
    acknowledge(&sender, 5, 80);
    gbn_sender_send(&sender, 81, &batch);
    TEST_ASSERT_EQUAL_UINT(1, batch.count);
    assert_packet(&batch.packets[0], RDT_FIN, 5, NULL, 0);
    TEST_ASSERT_EQUAL_UINT64(181, sender.deadline_ms);
    acknowledge(&sender, 6, 90);
    TEST_ASSERT_EQUAL_INT(RDT_DONE, sender.status);
    gbn_sender_send(&sender, 100, &batch);
    TEST_ASSERT_EQUAL_UINT(0, batch.count);
}

static void test_sender_empty_file_and_exact_payload_multiple(void)
{
    uint8_t bytes[2 * RDT_PAYLOAD_SIZE] = {0};
    gbn_sender sender;
    gbn_batch batch;
    TEST_ASSERT_TRUE(gbn_sender_init(&sender, NULL, 0, 64, 100));
    gbn_sender_send(&sender, 0, &batch);
    TEST_ASSERT_EQUAL_UINT(1, batch.count);
    assert_packet(&batch.packets[0], RDT_FIN, 0, NULL, 0);
    acknowledge(&sender, 1, 1);
    TEST_ASSERT_EQUAL_INT(RDT_DONE, sender.status);
    TEST_ASSERT_TRUE(gbn_sender_init(&sender, bytes, sizeof(bytes), 64, 100));
    gbn_sender_send(&sender, 0, &batch);
    TEST_ASSERT_EQUAL_UINT(2, batch.count);
    TEST_ASSERT_EQUAL_UINT16(RDT_PAYLOAD_SIZE, batch.packets[0].length);
    TEST_ASSERT_EQUAL_UINT16(RDT_PAYLOAD_SIZE, batch.packets[1].length);
    acknowledge(&sender, 2, 1);
    gbn_sender_send(&sender, 1, &batch);
    TEST_ASSERT_EQUAL_UINT(1, batch.count);
    assert_packet(&batch.packets[0], RDT_FIN, 2, NULL, 0);
}

static void test_sender_ignores_duplicate_future_and_non_ack_packets(void)
{
    uint8_t bytes[3 * RDT_PAYLOAD_SIZE] = {0};
    gbn_sender sender;
    gbn_batch batch;
    rdt_packet wrong;
    TEST_ASSERT_TRUE(gbn_sender_init(&sender, bytes, sizeof(bytes), 2, 100));
    gbn_sender_send(&sender, 5, &batch);
    acknowledge(&sender, 0, 50);
    acknowledge(&sender, 3, 50);
    acknowledge(&sender, UINT32_MAX, 50);
    wrong = packet(RDT_DATA, 1, "x", 1);
    gbn_sender_ack(&sender, &wrong, 50);
    wrong = packet(RDT_FIN, 1, NULL, 0);
    gbn_sender_ack(&sender, &wrong, 50);
    wrong = packet(RDT_ACK, 1, "x", 1);
    gbn_sender_ack(&sender, &wrong, 50);
    TEST_ASSERT_EQUAL_UINT32(0, sender.base);
    TEST_ASSERT_EQUAL_UINT64(105, sender.deadline_ms);
    acknowledge(&sender, 1, 60);
    acknowledge(&sender, 1, 90);
    acknowledge(&sender, 0, 90);
    TEST_ASSERT_EQUAL_UINT32(1, sender.base);
    TEST_ASSERT_EQUAL_UINT64(160, sender.deadline_ms);
}

static void test_sender_timer_retransmits_entire_unacknowledged_window(void)
{
    uint8_t bytes[3 * RDT_PAYLOAD_SIZE + 1] = {0};
    gbn_sender sender;
    gbn_batch first, resent;
    TEST_ASSERT_TRUE(gbn_sender_init(&sender, bytes, sizeof(bytes), 3, 100));
    gbn_sender_timeout(&sender, 10000, &resent);
    TEST_ASSERT_EQUAL_UINT(0, resent.count);
    gbn_sender_send(&sender, 20, &first);
    gbn_sender_timeout(&sender, 119, &resent);
    TEST_ASSERT_EQUAL_UINT(0, resent.count);
    TEST_ASSERT_EQUAL_UINT(0, sender.consecutive_timeouts);
    gbn_sender_timeout(&sender, 120, &resent);
    TEST_ASSERT_EQUAL_UINT(3, resent.count);
    TEST_ASSERT_EQUAL_UINT(1, sender.consecutive_timeouts);
    TEST_ASSERT_EQUAL_UINT64(220, sender.deadline_ms);
    for (size_t i = 0; i < first.count; ++i) {
        assert_packet(&resent.packets[i], first.packets[i].type, first.packets[i].seq,
                      first.packets[i].payload, first.packets[i].length);
    }
    acknowledge(&sender, 2, 150);
    TEST_ASSERT_EQUAL_UINT(0, sender.consecutive_timeouts);
    gbn_sender_timeout(&sender, 249, &resent);
    TEST_ASSERT_EQUAL_UINT(0, resent.count);
    gbn_sender_timeout(&sender, 250, &resent);
    TEST_ASSERT_EQUAL_UINT(1, resent.count);
    assert_packet(&resent.packets[0], RDT_DATA, 2, bytes + 2 * RDT_PAYLOAD_SIZE, RDT_PAYLOAD_SIZE);
}

static void test_sender_fails_after_ten_consecutive_timeouts_and_resets_on_progress(void)
{
    uint8_t bytes[2 * RDT_PAYLOAD_SIZE] = {0};
    gbn_sender sender;
    gbn_batch batch;
    TEST_ASSERT_TRUE(gbn_sender_init(&sender, bytes, sizeof(bytes), 2, 10));
    gbn_sender_send(&sender, 0, &batch);
    for (unsigned i = 1; i < RDT_MAX_TIMEOUTS; ++i) {
        gbn_sender_timeout(&sender, i * 10U, &batch);
        TEST_ASSERT_EQUAL_INT(RDT_ACTIVE, sender.status);
        TEST_ASSERT_EQUAL_UINT(2, batch.count);
    }
    acknowledge(&sender, 1, 95);
    TEST_ASSERT_EQUAL_UINT(0, sender.consecutive_timeouts);
    acknowledge(&sender, 1, 100);
    for (unsigned i = 1; i <= RDT_MAX_TIMEOUTS; ++i) {
        gbn_sender_timeout(&sender, 95U + i * 10U, &batch);
        TEST_ASSERT_EQUAL_UINT(i, sender.consecutive_timeouts);
        if (i < RDT_MAX_TIMEOUTS) {
            TEST_ASSERT_EQUAL_INT(RDT_ACTIVE, sender.status);
            TEST_ASSERT_EQUAL_UINT(1, batch.count);
        } else {
            TEST_ASSERT_EQUAL_INT(RDT_FAILED, sender.status);
            TEST_ASSERT_EQUAL_UINT(0, batch.count);
        }
    }
    acknowledge(&sender, 2, 200);
    TEST_ASSERT_EQUAL_INT(RDT_FAILED, sender.status);
    gbn_sender_send(&sender, 200, &batch);
    TEST_ASSERT_EQUAL_UINT(0, batch.count);
    gbn_sender_timeout(&sender, 200, &batch);
    TEST_ASSERT_EQUAL_UINT(0, batch.count);
}

static void test_sender_fin_retransmission_and_ring_buffer_reuse(void)
{
    uint8_t bytes[5 * RDT_PAYLOAD_SIZE];
    gbn_sender sender;
    gbn_batch batch;
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        bytes[i] = (uint8_t)(i / RDT_PAYLOAD_SIZE + 17U);
    }
    TEST_ASSERT_TRUE(gbn_sender_init(&sender, bytes, sizeof(bytes), 2, 10));
    gbn_sender_send(&sender, 0, &batch);
    acknowledge(&sender, 2, 1);
    gbn_sender_send(&sender, 2, &batch);
    acknowledge(&sender, 3, 3);
    gbn_sender_send(&sender, 4, &batch);
    TEST_ASSERT_EQUAL_UINT(1, batch.count);
    assert_packet(&batch.packets[0], RDT_DATA, 4, bytes + 4 * RDT_PAYLOAD_SIZE, RDT_PAYLOAD_SIZE);
    gbn_sender_timeout(&sender, 13, &batch);
    TEST_ASSERT_EQUAL_UINT(2, batch.count);
    assert_packet(&batch.packets[0], RDT_DATA, 3, bytes + 3 * RDT_PAYLOAD_SIZE, RDT_PAYLOAD_SIZE);
    assert_packet(&batch.packets[1], RDT_DATA, 4, bytes + 4 * RDT_PAYLOAD_SIZE, RDT_PAYLOAD_SIZE);
    acknowledge(&sender, 5, 14);
    gbn_sender_send(&sender, 15, &batch);
    TEST_ASSERT_EQUAL_UINT(1, batch.count);
    assert_packet(&batch.packets[0], RDT_FIN, 5, NULL, 0);
    gbn_sender_send(&sender, 16, &batch);
    TEST_ASSERT_EQUAL_UINT(0, batch.count);
    gbn_sender_timeout(&sender, 25, &batch);
    TEST_ASSERT_EQUAL_UINT(1, batch.count);
    assert_packet(&batch.packets[0], RDT_FIN, 5, NULL, 0);
    acknowledge(&sender, 6, 26);
    TEST_ASSERT_EQUAL_INT(RDT_DONE, sender.status);
    gbn_sender_timeout(&sender, 100, &batch);
    TEST_ASSERT_EQUAL_UINT(0, batch.count);
}

static void test_receiver_delivers_in_order_and_repeats_cumulative_ack(void)
{
    gbn_receiver receiver;
    gbn_receive_action action;
    rdt_packet p = packet(RDT_DATA, 1, "second", 6);
    gbn_receiver_init(&receiver, 10);
    TEST_ASSERT_EQUAL_INT(RDT_ACTIVE, receiver.status);
    gbn_receiver_receive(&receiver, &p, 20, &action);
    TEST_ASSERT_TRUE(action.ack);
    assert_packet(&action.packet, RDT_ACK, 0, NULL, 0);
    TEST_ASSERT_NULL(action.deliver);
    TEST_ASSERT_EQUAL_UINT(0, action.size);
    TEST_ASSERT_FALSE(action.finish);
    TEST_ASSERT_EQUAL_UINT32(0, receiver.expected);
    p = packet(RDT_DATA, 0, "first", 5);
    gbn_receiver_receive(&receiver, &p, 30, &action);
    TEST_ASSERT_TRUE(action.ack);
    assert_packet(&action.packet, RDT_ACK, 1, NULL, 0);
    TEST_ASSERT_EQUAL_MEMORY("first", action.deliver, action.size);
    TEST_ASSERT_EQUAL_UINT(5, action.size);
    TEST_ASSERT_EQUAL_UINT(5, receiver.received_size);
    TEST_ASSERT_EQUAL_UINT64(30, receiver.last_activity_ms);
    gbn_receiver_receive(&receiver, &p, 40, &action);
    TEST_ASSERT_TRUE(action.ack);
    TEST_ASSERT_EQUAL_UINT32(1, action.packet.seq);
    TEST_ASSERT_NULL(action.deliver);
    TEST_ASSERT_EQUAL_UINT(5, receiver.received_size);
    p = packet(RDT_DATA, 1, "second", 6);
    gbn_receiver_receive(&receiver, &p, 50, &action);
    TEST_ASSERT_EQUAL_UINT32(2, action.packet.seq);
    TEST_ASSERT_EQUAL_MEMORY("second", action.deliver, 6);
    TEST_ASSERT_EQUAL_UINT(11, receiver.received_size);
}

static void test_receiver_fin_must_be_in_order_and_linger_is_fixed(void)
{
    gbn_receiver receiver;
    gbn_receive_action action;
    rdt_packet p = packet(RDT_FIN, 1, NULL, 0);
    gbn_receiver_init(&receiver, 0);
    gbn_receiver_receive(&receiver, &p, 10, &action);
    TEST_ASSERT_TRUE(action.ack);
    TEST_ASSERT_EQUAL_UINT32(0, action.packet.seq);
    TEST_ASSERT_FALSE(action.finish);
    TEST_ASSERT_FALSE(receiver.finishing);
    p = packet(RDT_DATA, 0, "abc", 3);
    gbn_receiver_receive(&receiver, &p, 20, &action);
    p = packet(RDT_FIN, 1, NULL, 0);
    gbn_receiver_receive(&receiver, &p, 30, &action);
    TEST_ASSERT_TRUE(action.finish);
    TEST_ASSERT_TRUE(action.ack);
    TEST_ASSERT_EQUAL_UINT32(2, action.packet.seq);
    TEST_ASSERT_NULL(action.deliver);
    TEST_ASSERT_TRUE(receiver.finishing);
    TEST_ASSERT_EQUAL_UINT64(30 + RDT_LINGER_MS, receiver.linger_until_ms);
    TEST_ASSERT_EQUAL_INT(RDT_ACTIVE, gbn_receiver_tick(&receiver, 2029));
    gbn_receiver_receive(&receiver, &p, 2029, &action);
    TEST_ASSERT_TRUE(action.ack);
    TEST_ASSERT_EQUAL_UINT32(2, action.packet.seq);
    TEST_ASSERT_FALSE(action.finish);
    TEST_ASSERT_EQUAL_UINT64(2030, receiver.linger_until_ms);
    p = packet(RDT_DATA, 1, "ignored", 7);
    gbn_receiver_receive(&receiver, &p, 2029, &action);
    TEST_ASSERT_NULL(action.deliver);
    TEST_ASSERT_FALSE(action.finish);
    TEST_ASSERT_EQUAL_UINT(3, receiver.received_size);
    p = packet(RDT_FIN, 2, NULL, 0);
    gbn_receiver_receive(&receiver, &p, 2029, &action);
    TEST_ASSERT_FALSE(action.finish);
    TEST_ASSERT_EQUAL_INT(RDT_DONE, gbn_receiver_tick(&receiver, 2030));
    gbn_receiver_receive(&receiver, &p, 2031, &action);
    TEST_ASSERT_FALSE(action.ack);
    TEST_ASSERT_NULL(action.deliver);
    TEST_ASSERT_FALSE(action.finish);
    TEST_ASSERT_EQUAL_INT(RDT_DONE, gbn_receiver_tick(&receiver, 99999));
}

static void test_receiver_empty_file_and_inactivity_boundary(void)
{
    gbn_receiver receiver;
    gbn_receive_action action;
    rdt_packet p = packet(RDT_FIN, 0, NULL, 0);
    gbn_receiver_init(&receiver, 100);
    TEST_ASSERT_EQUAL_INT(RDT_ACTIVE, gbn_receiver_tick(&receiver, 100 + RDT_RECEIVER_IDLE_MS - 1));
    TEST_ASSERT_EQUAL_INT(RDT_FAILED, gbn_receiver_tick(&receiver, 100 + RDT_RECEIVER_IDLE_MS));
    gbn_receiver_receive(&receiver, &p, 30101, &action);
    TEST_ASSERT_FALSE(action.ack);
    TEST_ASSERT_FALSE(action.finish);
    TEST_ASSERT_EQUAL_INT(RDT_FAILED, gbn_receiver_tick(&receiver, 100000));
    gbn_receiver_init(&receiver, 0);
    gbn_receiver_receive(&receiver, &p, 1, &action);
    TEST_ASSERT_TRUE(action.finish);
    TEST_ASSERT_TRUE(action.ack);
    TEST_ASSERT_EQUAL_UINT32(1, action.packet.seq);
    TEST_ASSERT_EQUAL_UINT(0, receiver.received_size);
    TEST_ASSERT_EQUAL_INT(RDT_DONE, gbn_receiver_tick(&receiver, 1 + RDT_LINGER_MS));
}

static void test_receiver_valid_activity_resets_idle_timer(void)
{
    gbn_receiver receiver;
    gbn_receive_action action;
    rdt_packet p = packet(RDT_DATA, 2, "gap", 3);
    gbn_receiver_init(&receiver, 100);
    gbn_receiver_receive(&receiver, &p, 30099, &action);
    TEST_ASSERT_TRUE(action.ack);
    TEST_ASSERT_EQUAL_UINT64(30099, receiver.last_activity_ms);
    TEST_ASSERT_EQUAL_INT(RDT_ACTIVE, gbn_receiver_tick(&receiver, 60098));
    TEST_ASSERT_EQUAL_INT(RDT_FAILED, gbn_receiver_tick(&receiver, 60099));
}

static void test_receiver_rejects_invalid_semantic_packets(void)
{
    gbn_receiver receiver;
    gbn_receive_action action;
    rdt_packet p = packet(RDT_ACK, 0, NULL, 0);
    gbn_receiver_init(&receiver, 100);
    gbn_receiver_receive(&receiver, &p, 200, &action);
    TEST_ASSERT_FALSE(action.ack);
    TEST_ASSERT_NULL(action.deliver);
    TEST_ASSERT_EQUAL_UINT64(100, receiver.last_activity_ms);
    p.type = 3;
    gbn_receiver_receive(&receiver, &p, 200, &action);
    TEST_ASSERT_FALSE(action.ack);
    p.type = RDT_DATA;
    p.length = RDT_PAYLOAD_SIZE + 1;
    gbn_receiver_receive(&receiver, &p, 200, &action);
    TEST_ASSERT_FALSE(action.ack);
    p.type = RDT_FIN;
    p.length = 1;
    gbn_receiver_receive(&receiver, &p, 200, &action);
    TEST_ASSERT_FALSE(action.ack);
    TEST_ASSERT_FALSE(action.finish);
    TEST_ASSERT_EQUAL_UINT64(100, receiver.last_activity_ms);
}

static void test_receiver_refuses_file_above_limit(void)
{
    gbn_receiver receiver;
    gbn_receive_action action;
    rdt_packet p = packet(RDT_DATA, 0, "x", 1);
    gbn_receiver_init(&receiver, 0);
    receiver.received_size = RDT_MAX_FILE_SIZE;
    gbn_receiver_receive(&receiver, &p, 1, &action);
    TEST_ASSERT_EQUAL_INT(RDT_FAILED, receiver.status);
    TEST_ASSERT_FALSE(action.ack);
    TEST_ASSERT_NULL(action.deliver);
    TEST_ASSERT_EQUAL_UINT(RDT_MAX_FILE_SIZE, receiver.received_size);
    gbn_receiver_init(&receiver, 0);
    receiver.received_size = RDT_MAX_FILE_SIZE - 1;
    gbn_receiver_receive(&receiver, &p, 1, &action);
    TEST_ASSERT_EQUAL_INT(RDT_ACTIVE, receiver.status);
    TEST_ASSERT_EQUAL_UINT(RDT_MAX_FILE_SIZE, receiver.received_size);
    TEST_ASSERT_EQUAL_UINT(1, action.size);
}

static void test_public_state_machine_null_arguments_are_safe(void)
{
    gbn_sender sender;
    gbn_receiver receiver;
    gbn_batch batch;
    gbn_receive_action action;
    rdt_packet p = packet(RDT_FIN, 0, NULL, 0);
    TEST_ASSERT_TRUE(gbn_sender_init(&sender, NULL, 0, 1, 10));
    gbn_sender_send(&sender, 0, NULL);
    TEST_ASSERT_EQUAL_UINT32(0, sender.next);
    batch.count = 7;
    gbn_sender_send(NULL, 0, &batch);
    TEST_ASSERT_EQUAL_UINT(0, batch.count);
    gbn_sender_timeout(&sender, 0, NULL);
    batch.count = 7;
    gbn_sender_timeout(NULL, 0, &batch);
    TEST_ASSERT_EQUAL_UINT(0, batch.count);
    gbn_sender_ack(NULL, &p, 0);
    gbn_sender_ack(&sender, NULL, 0);
    TEST_ASSERT_EQUAL_UINT32(0, sender.base);

    gbn_receiver_init(NULL, 0);
    TEST_ASSERT_EQUAL_INT(RDT_FAILED, gbn_receiver_tick(NULL, 0));
    gbn_receiver_init(&receiver, 0);
    gbn_receiver_receive(&receiver, &p, 0, NULL);
    TEST_ASSERT_FALSE(receiver.finishing);
    memset(&action, 0xff, sizeof(action));
    gbn_receiver_receive(NULL, &p, 0, &action);
    TEST_ASSERT_FALSE(action.ack);
    TEST_ASSERT_NULL(action.deliver);
    TEST_ASSERT_FALSE(action.finish);
    memset(&action, 0xff, sizeof(action));
    gbn_receiver_receive(&receiver, NULL, 0, &action);
    TEST_ASSERT_FALSE(action.ack);
    TEST_ASSERT_NULL(action.deliver);
}

static void test_virtual_clock_boundaries_do_not_wrap(void)
{
    gbn_sender sender;
    gbn_receiver receiver;
    gbn_batch batch;
    gbn_receive_action action;
    rdt_packet fin = packet(RDT_FIN, 0, NULL, 0);
    TEST_ASSERT_TRUE(gbn_sender_init(&sender, NULL, 0, 1, 10));
    gbn_sender_send(&sender, UINT64_MAX - 5U, &batch);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, sender.deadline_ms);
    gbn_sender_timeout(&sender, UINT64_MAX - 1U, &batch);
    TEST_ASSERT_EQUAL_UINT(0, batch.count);
    gbn_sender_timeout(&sender, UINT64_MAX, &batch);
    TEST_ASSERT_EQUAL_UINT(1, batch.count);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, sender.deadline_ms);

    gbn_receiver_init(&receiver, 100);
    TEST_ASSERT_EQUAL_INT(RDT_ACTIVE, gbn_receiver_tick(&receiver, 99));
    gbn_receiver_init(&receiver, UINT64_MAX - 1000U);
    gbn_receiver_receive(&receiver, &fin, UINT64_MAX - 1000U, &action);
    TEST_ASSERT_TRUE(action.finish);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, receiver.linger_until_ms);
    TEST_ASSERT_EQUAL_INT(RDT_ACTIVE, gbn_receiver_tick(&receiver, UINT64_MAX - 1U));
    TEST_ASSERT_EQUAL_INT(RDT_DONE, gbn_receiver_tick(&receiver, UINT64_MAX));
}

/* Loss, corruption and duplication are selected independently with 20%
 * probability in each direction, including FIN and ACK. Delivery is delayed
 * by a few virtual milliseconds, also exercising reordering and stale ACKs. */
#define CHANNEL_CAPACITY 4096U
typedef struct {
    bool to_receiver;
    uint64_t due;
    size_t size;
    uint8_t wire[RDT_MAX_PACKET_SIZE];
} channel_event;
typedef struct {
    channel_event *events;
    size_t count;
    uint32_t random;
    unsigned dropped[2], corrupted[2], duplicated[2];
} memory_channel;

static uint32_t next_random(memory_channel *channel)
{
    uint32_t value = channel->random;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    channel->random = value;
    return value;
}

static void channel_send(memory_channel *channel, const rdt_packet *p,
                         bool to_receiver, uint64_t now, bool impair)
{
    channel_event event;
    unsigned direction = to_receiver ? 0U : 1U;
    bool drop = impair && next_random(channel) % 5U == 0;
    bool corrupt = impair && next_random(channel) % 5U == 0;
    bool duplicate = impair && next_random(channel) % 5U == 0;
    event.to_receiver = to_receiver;
    event.due = now + 1U + next_random(channel) % 4U;
    event.size = rdt_packet_encode(p, event.wire, sizeof(event.wire));
    TEST_ASSERT_NOT_EQUAL(0, event.size);
    if (drop) {
        ++channel->dropped[direction];
        return;
    }
    if (corrupt) {
        ++channel->corrupted[direction];
        event.wire[next_random(channel) % event.size] ^= 0x40;
    }
    TEST_ASSERT_LESS_THAN(CHANNEL_CAPACITY, channel->count);
    channel->events[channel->count++] = event;
    if (duplicate) {
        ++channel->duplicated[direction];
        event.due += 1;
        TEST_ASSERT_LESS_THAN(CHANNEL_CAPACITY, channel->count);
        channel->events[channel->count++] = event;
    }
}

static void simulate_transfer(uint32_t seed, size_t size, size_t window,
                              bool impair, bool check_damage)
{
    uint8_t *input = malloc(size == 0 ? 1 : size);
    uint8_t *output = malloc(size == 0 ? 1 : size);
    memory_channel channel;
    gbn_sender sender;
    gbn_receiver receiver;
    gbn_batch batch;
    size_t delivered = 0;
    unsigned finishes = 0;
    memset(&channel, 0, sizeof(channel));
    channel.events = calloc(CHANNEL_CAPACITY, sizeof(*channel.events));
    channel.random = seed;
    TEST_ASSERT_NOT_NULL(input);
    TEST_ASSERT_NOT_NULL(output);
    TEST_ASSERT_NOT_NULL(channel.events);
    for (size_t i = 0; i < size; ++i) {
        input[i] = (uint8_t)(i * 37U + i / 251U);
    }
    TEST_ASSERT_TRUE(gbn_sender_init(&sender, input, size, window, 100));
    gbn_receiver_init(&receiver, 0);
    for (uint64_t now = 0; now < 120000; ++now) {
        gbn_sender_send(&sender, now, &batch);
        for (size_t i = 0; i < batch.count; ++i) {
            channel_send(&channel, &batch.packets[i], true, now, impair);
        }
        gbn_sender_timeout(&sender, now, &batch);
        for (size_t i = 0; i < batch.count; ++i) {
            channel_send(&channel, &batch.packets[i], true, now, impair);
        }
        for (size_t i = 0; i < channel.count;) {
            channel_event event;
            rdt_packet decoded;
            if (channel.events[i].due > now) {
                ++i;
                continue;
            }
            event = channel.events[i];
            channel.events[i] = channel.events[--channel.count];
            if (!rdt_packet_decode(event.wire, event.size, &decoded)) {
                continue;
            }
            if (event.to_receiver) {
                gbn_receive_action action;
                gbn_receiver_receive(&receiver, &decoded, now, &action);
                if (action.deliver != NULL) {
                    TEST_ASSERT_LESS_OR_EQUAL(size, delivered + action.size);
                    memcpy(output + delivered, action.deliver, action.size);
                    delivered += action.size;
                }
                if (action.finish) {
                    ++finishes;
                }
                if (action.ack) {
                    channel_send(&channel, &action.packet, false, now, impair);
                }
            } else {
                gbn_sender_ack(&sender, &decoded, now);
            }
        }
        gbn_receiver_tick(&receiver, now);
        if (sender.status != RDT_ACTIVE && receiver.status != RDT_ACTIVE) {
            break;
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(RDT_DONE, sender.status, "fixed-seed sender did not complete");
    TEST_ASSERT_EQUAL_INT_MESSAGE(RDT_DONE, receiver.status, "fixed-seed receiver did not complete");
    TEST_ASSERT_EQUAL_UINT(size, delivered);
    TEST_ASSERT_EQUAL_UINT(1, finishes);
    if (size != 0) {
        TEST_ASSERT_EQUAL_MEMORY(input, output, size);
    }
    if (check_damage) {
        for (unsigned i = 0; i < 2; ++i) {
            TEST_ASSERT_GREATER_THAN(0, channel.dropped[i]);
            TEST_ASSERT_GREATER_THAN(0, channel.corrupted[i]);
            TEST_ASSERT_GREATER_THAN(0, channel.duplicated[i]);
        }
    }
    free(channel.events);
    free(output);
    free(input);
}

static void test_in_memory_channel_clean_empty_and_exact_multiple_files(void)
{
    simulate_transfer(1, 0, 1, false, false);
    simulate_transfer(2, 1, 1, false, false);
    simulate_transfer(3, 1024, 4, false, false);
    simulate_transfer(4, 2048, 64, false, false);
    simulate_transfer(5, 3 * 1024 + 7, 2, false, false);
}

static void test_in_memory_channel_fixed_seed_bidirectional_damage(void)
{
    simulate_transfer(UINT32_C(0x12345678), 32 * 1024 + 19, 8, true, true);
    simulate_transfer(UINT32_C(0x9abcdef0), 48 * 1024, 16, true, true);
    simulate_transfer(UINT32_C(0xdeadbeef), 64 * 1024 + 3, 64, true, true);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_checksum_rfc1071_and_odd_lengths);
    RUN_TEST(test_exact_wire_fixtures_and_network_order);
    RUN_TEST(test_packet_roundtrips_boundary_payloads);
    RUN_TEST(test_packet_encoder_rejects_invalid_arguments);
    RUN_TEST(test_packet_decoder_rejects_each_malformed_wire_condition);
    RUN_TEST(test_sender_initialization_limits);
    RUN_TEST(test_sender_window_slides_with_cumulative_ack);
    RUN_TEST(test_sender_empty_file_and_exact_payload_multiple);
    RUN_TEST(test_sender_ignores_duplicate_future_and_non_ack_packets);
    RUN_TEST(test_sender_timer_retransmits_entire_unacknowledged_window);
    RUN_TEST(test_sender_fails_after_ten_consecutive_timeouts_and_resets_on_progress);
    RUN_TEST(test_sender_fin_retransmission_and_ring_buffer_reuse);
    RUN_TEST(test_receiver_delivers_in_order_and_repeats_cumulative_ack);
    RUN_TEST(test_receiver_fin_must_be_in_order_and_linger_is_fixed);
    RUN_TEST(test_receiver_empty_file_and_inactivity_boundary);
    RUN_TEST(test_receiver_valid_activity_resets_idle_timer);
    RUN_TEST(test_receiver_rejects_invalid_semantic_packets);
    RUN_TEST(test_receiver_refuses_file_above_limit);
    RUN_TEST(test_public_state_machine_null_arguments_are_safe);
    RUN_TEST(test_virtual_clock_boundaries_do_not_wrap);
    RUN_TEST(test_in_memory_channel_clean_empty_and_exact_multiple_files);
    RUN_TEST(test_in_memory_channel_fixed_seed_bidirectional_damage);
    return UNITY_END();
}
