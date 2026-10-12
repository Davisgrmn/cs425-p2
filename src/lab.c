#include "lab.h"

#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

static bool valid_packet(const rdt_packet *packet)
{
    return packet != NULL && packet->type <= RDT_FIN &&
           packet->length <= RDT_PAYLOAD_SIZE &&
           (packet->type == RDT_DATA || packet->length == 0U);
}

static uint64_t deadline_after(uint64_t now_ms, uint64_t delay_ms)
{
    return delay_ms > UINT64_MAX - now_ms ? UINT64_MAX : now_ms + delay_ms;
}

uint16_t rdt_checksum(const uint8_t *bytes, size_t size)
{
    uint32_t sum = 0U;

    if (bytes == NULL && size != 0U) {
        return 0U;
    }
    while (size >= 2U) {
        sum += ((uint32_t)bytes[0] << 8U) | (uint32_t)bytes[1];
        /* Fold each word so arbitrarily large inputs cannot overflow. */
        sum = (sum & UINT32_C(0xffff)) + (sum >> 16U);
        bytes += 2;
        size -= 2U;
    }
    if (size != 0U) {
        sum += (uint32_t)bytes[0] << 8U;
    }
    while ((sum >> 16U) != 0U) {
        sum = (sum & UINT32_C(0xffff)) + (sum >> 16U);
    }
    return (uint16_t)(~sum & UINT32_C(0xffff));
}

size_t rdt_packet_encode(const rdt_packet *packet, uint8_t *bytes,
                         size_t capacity)
{
    size_t size;
    uint16_t checksum;
    uint16_t length;
    uint32_t sequence;

    if (!valid_packet(packet) || bytes == NULL) {
        return 0U;
    }
    size = RDT_HEADER_SIZE + (size_t)packet->length;
    if (capacity < size) {
        return 0U;
    }

    bytes[0] = packet->type;
    bytes[1] = 0U;
    bytes[2] = 0U;
    bytes[3] = 0U;
    sequence = htonl(packet->seq);
    length = htons(packet->length);
    memcpy(bytes + 4U, &sequence, sizeof(sequence));
    memcpy(bytes + 8U, &length, sizeof(length));
    if (packet->length != 0U) {
        memcpy(bytes + RDT_HEADER_SIZE, packet->payload, packet->length);
    }
    checksum = htons(rdt_checksum(bytes, size));
    memcpy(bytes + 2U, &checksum, sizeof(checksum));
    return size;
}

bool rdt_packet_decode(const uint8_t *bytes, size_t size, rdt_packet *packet)
{
    rdt_packet decoded;
    uint16_t length;
    uint32_t sequence;

    if (bytes == NULL || packet == NULL || size < RDT_HEADER_SIZE ||
        bytes[0] > RDT_FIN || bytes[1] != 0U) {
        return false;
    }
    memcpy(&length, bytes + 8U, sizeof(length));
    length = ntohs(length);
    if (length > RDT_PAYLOAD_SIZE ||
        size != RDT_HEADER_SIZE + (size_t)length ||
        (bytes[0] != RDT_DATA && length != 0U) ||
        rdt_checksum(bytes, size) != 0U) {
        return false;
    }

    memset(&decoded, 0, sizeof(decoded));
    decoded.type = bytes[0];
    memcpy(&sequence, bytes + 4U, sizeof(sequence));
    decoded.seq = ntohl(sequence);
    decoded.length = length;
    if (length != 0U) {
        memcpy(decoded.payload, bytes + RDT_HEADER_SIZE, length);
    }
    *packet = decoded;
    return true;
}

bool gbn_sender_init(gbn_sender *sender, const uint8_t *data, size_t size,
                     size_t window, uint64_t timeout_ms)
{
    if (sender == NULL) {
        return false;
    }
    memset(sender, 0, sizeof(*sender));
    sender->status = RDT_FAILED;
    if ((data == NULL && size != 0U) || size > RDT_MAX_FILE_SIZE ||
        window == 0U || window > RDT_MAX_WINDOW || timeout_ms == 0U) {
        return false;
    }
    sender->data = data;
    sender->size = size;
    sender->window = window;
    sender->timeout_ms = timeout_ms;
    sender->data_count = (uint32_t)((size + RDT_PAYLOAD_SIZE - 1U) /
                                   RDT_PAYLOAD_SIZE);
    sender->status = RDT_ACTIVE;
    return true;
}

void gbn_sender_send(gbn_sender *sender, uint64_t now_ms, gbn_batch *batch)
{
    if (batch == NULL) {
        return;
    }
    batch->count = 0U;
    if (sender == NULL || sender->status != RDT_ACTIVE) {
        return;
    }

    while (sender->next < sender->data_count &&
           (size_t)(sender->next - sender->base) < sender->window) {
        rdt_packet *packet = &sender->saved[sender->next % RDT_MAX_WINDOW];
        const size_t offset = (size_t)sender->next * RDT_PAYLOAD_SIZE;
        const size_t remaining = sender->size - offset;
        const size_t length = remaining < RDT_PAYLOAD_SIZE ?
                              remaining : RDT_PAYLOAD_SIZE;

        memset(packet, 0, sizeof(*packet));
        packet->type = RDT_DATA;
        packet->seq = sender->next;
        packet->length = (uint16_t)length;
        memcpy(packet->payload, sender->data + offset, length);
        if (sender->base == sender->next) {
            sender->deadline_ms = deadline_after(now_ms, sender->timeout_ms);
        }
        batch->packets[batch->count++] = *packet;
        ++sender->next;
    }

    /* FIN must wait for cumulative acknowledgement of every DATA packet. */
    if (sender->base == sender->data_count &&
        sender->next == sender->data_count) {
        rdt_packet *packet = &sender->saved[sender->next % RDT_MAX_WINDOW];

        memset(packet, 0, sizeof(*packet));
        packet->type = RDT_FIN;
        packet->seq = sender->data_count;
        batch->packets[batch->count++] = *packet;
        ++sender->next;
        sender->deadline_ms = deadline_after(now_ms, sender->timeout_ms);
    }
}

void gbn_sender_ack(gbn_sender *sender, const rdt_packet *packet,
                    uint64_t now_ms)
{
    if (sender == NULL || sender->status != RDT_ACTIVE ||
        !valid_packet(packet) || packet->type != RDT_ACK ||
        packet->seq <= sender->base || packet->seq > sender->next) {
        return;
    }
    sender->base = packet->seq;
    sender->consecutive_timeouts = 0U;
    sender->deadline_ms = sender->base < sender->next ?
        deadline_after(now_ms, sender->timeout_ms) : 0U;
    if (sender->base == sender->data_count + 1U) {
        sender->status = RDT_DONE;
    }
}

void gbn_sender_timeout(gbn_sender *sender, uint64_t now_ms, gbn_batch *batch)
{
    uint32_t sequence;

    if (batch == NULL) {
        return;
    }
    batch->count = 0U;
    if (sender == NULL || sender->status != RDT_ACTIVE ||
        sender->base == sender->next || now_ms < sender->deadline_ms) {
        return;
    }
    ++sender->consecutive_timeouts;
    if (sender->consecutive_timeouts >= RDT_MAX_TIMEOUTS) {
        sender->status = RDT_FAILED;
        return;
    }
    for (sequence = sender->base; sequence < sender->next; ++sequence) {
        batch->packets[batch->count++] =
            sender->saved[sequence % RDT_MAX_WINDOW];
    }
    sender->deadline_ms = deadline_after(now_ms, sender->timeout_ms);
}

void gbn_receiver_init(gbn_receiver *receiver, uint64_t now_ms)
{
    if (receiver != NULL) {
        memset(receiver, 0, sizeof(*receiver));
        receiver->last_activity_ms = now_ms;
        receiver->status = RDT_ACTIVE;
    }
}

rdt_status gbn_receiver_tick(gbn_receiver *receiver, uint64_t now_ms)
{
    if (receiver == NULL) {
        return RDT_FAILED;
    }
    if (receiver->status == RDT_ACTIVE) {
        if (receiver->finishing) {
            if (now_ms >= receiver->linger_until_ms) {
                receiver->status = RDT_DONE;
            }
        } else if (now_ms >= receiver->last_activity_ms &&
                   now_ms - receiver->last_activity_ms >=
                   RDT_RECEIVER_IDLE_MS) {
            receiver->status = RDT_FAILED;
        }
    }
    return receiver->status;
}

static void acknowledge(const gbn_receiver *receiver,
                        gbn_receive_action *action)
{
    action->ack = true;
    action->packet.type = RDT_ACK;
    action->packet.seq = receiver->expected;
}

void gbn_receiver_receive(gbn_receiver *receiver, const rdt_packet *packet,
                          uint64_t now_ms, gbn_receive_action *action)
{
    if (action == NULL) {
        return;
    }
    memset(action, 0, sizeof(*action));
    if (receiver == NULL || gbn_receiver_tick(receiver, now_ms) != RDT_ACTIVE ||
        !valid_packet(packet) || packet->type == RDT_ACK) {
        return;
    }
    if (receiver->finishing) {
        if (packet->type == RDT_FIN && packet->seq + 1U == receiver->expected) {
            acknowledge(receiver, action);
        }
        return;
    }

    receiver->last_activity_ms = now_ms;
    if (packet->type == RDT_DATA && packet->seq == receiver->expected) {
        if ((size_t)packet->length > RDT_MAX_FILE_SIZE - receiver->received_size) {
            receiver->status = RDT_FAILED;
            return;
        }
        action->deliver = packet->payload;
        action->size = packet->length;
        receiver->received_size += packet->length;
        ++receiver->expected;
    } else if (packet->type == RDT_FIN && packet->seq == receiver->expected) {
        ++receiver->expected;
        receiver->finishing = true;
        receiver->linger_until_ms = deadline_after(now_ms, RDT_LINGER_MS);
        action->finish = true;
    }
    acknowledge(receiver, action);
}
