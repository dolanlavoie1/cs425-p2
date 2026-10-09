#include "lab.h"

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

uint16_t lab_checksum(const uint8_t *data, size_t length)
{
    uint32_t sum = 0;

    for (size_t i = 0; i + 1U < length; i += 2U) {
        sum += ((uint32_t) data[i] << 8) | data[i + 1U];
        sum = (sum & UINT16_MAX) + (sum >> 16);
    }
    if ((length % 2U) != 0U) {
        sum += (uint32_t) data[length - 1U] << 8;
    }
    while (sum > UINT16_MAX) {
        sum = (sum & UINT16_MAX) + (sum >> 16);
    }
    return (uint16_t) ~sum;
}

bool lab_encode_packet(const LabPacket *packet, uint8_t *buffer,
                       size_t capacity, size_t *encoded_length)
{
    size_t total;
    uint16_t checksum;

    if (packet == NULL || buffer == NULL || encoded_length == NULL ||
        packet->type > LAB_FIN || packet->length > LAB_MAX_PAYLOAD ||
        ((packet->type == LAB_ACK || packet->type == LAB_FIN) &&
         packet->length != 0U)) {
        return false;
    }
    total = LAB_PACKET_HEADER + packet->length;
    if (capacity < total) {
        return false;
    }
    memset(buffer, 0, total);
    buffer[0] = (uint8_t) packet->type;
    buffer[4] = (uint8_t) (packet->seq >> 24);
    buffer[5] = (uint8_t) (packet->seq >> 16);
    buffer[6] = (uint8_t) (packet->seq >> 8);
    buffer[7] = (uint8_t) packet->seq;
    buffer[8] = (uint8_t) (packet->length >> 8);
    buffer[9] = (uint8_t) packet->length;
    if (packet->length > 0U) {
        memcpy(buffer + LAB_PACKET_HEADER, packet->payload, packet->length);
    }
    checksum = lab_checksum(buffer, total);
    buffer[2] = (uint8_t) (checksum >> 8);
    buffer[3] = (uint8_t) checksum;
    *encoded_length = total;
    return true;
}

bool lab_decode_packet(const uint8_t *buffer, size_t length, LabPacket *packet)
{
    uint16_t payload_length;

    if (buffer == NULL || packet == NULL || length < LAB_PACKET_HEADER ||
        length > LAB_MAX_PACKET || buffer[0] > LAB_FIN || buffer[1] != 0U) {
        return false;
    }
    payload_length = ((uint16_t) buffer[8] << 8) | buffer[9];
    if (payload_length > LAB_MAX_PAYLOAD ||
        LAB_PACKET_HEADER + (size_t) payload_length != length ||
        lab_checksum(buffer, length) != 0U) {
        return false;
    }
    if ((buffer[0] == LAB_ACK || buffer[0] == LAB_FIN) &&
        payload_length != 0U) {
        return false;
    }
    packet->type = (LabPacketType) buffer[0];
    packet->seq = ((uint32_t) buffer[4] << 24) |
                 ((uint32_t) buffer[5] << 16) |
                 ((uint32_t) buffer[6] << 8) | buffer[7];
    packet->length = payload_length;
    if (payload_length > 0U) {
        memcpy(packet->payload, buffer + LAB_PACKET_HEADER, payload_length);
    }
    return true;
}

static LabPacket sender_packet(const LabSender *sender, uint32_t sequence)
{
    LabPacket packet = {0};
    size_t offset;
    size_t remaining;

    packet.seq = sequence;
    if (sequence < sender->data_packets) {
        packet.type = LAB_DATA;
        offset = (size_t) sequence * LAB_MAX_PAYLOAD;
        remaining = sender->size - offset;
        packet.length = (uint16_t) (remaining > LAB_MAX_PAYLOAD ?
                                    LAB_MAX_PAYLOAD : remaining);
        if (packet.length > 0U) {
            memcpy(packet.payload, sender->data + offset, packet.length);
        }
    } else {
        packet.type = LAB_FIN;
    }
    return packet;
}

static size_t sender_emit(LabSender *sender, LabAction *actions, size_t capacity)
{
    size_t count = 0;

    while (sender->next < sender->base + sender->window &&
           sender->next < sender->data_packets && count < capacity) {
        actions[count].type = LAB_ACTION_SEND;
        actions[count].packet = sender_packet(sender, sender->next);
        count++;
        sender->next++;
    }
    if (sender->base == sender->data_packets &&
        sender->next == sender->base && count < capacity) {
        actions[count].type = LAB_ACTION_SEND;
        actions[count].packet = sender_packet(sender, sender->next);
        count++;
        sender->next++;
    }
    return count;
}

static void sender_arm(LabSender *sender, uint64_t now_ms)
{
    sender->timer_due_ms = now_ms + sender->timeout_ms;
    sender->timer_active = sender->base < sender->next;
}

void lab_sender_init(LabSender *sender, const uint8_t *data, size_t size,
                     uint32_t window, uint32_t timeout_ms)
{
    memset(sender, 0, sizeof(*sender));
    sender->data = data;
    sender->size = size;
    sender->data_packets = (uint32_t) ((size + LAB_MAX_PAYLOAD - 1U) /
                                       LAB_MAX_PAYLOAD);
    sender->window = window < 1U ? 1U :
                     (window > LAB_MAX_WINDOW ? LAB_MAX_WINDOW : window);
    sender->timeout_ms = timeout_ms == 0U ? 1U : timeout_ms;
}

size_t lab_sender_start(LabSender *sender, uint64_t now_ms,
                        LabAction *actions, size_t capacity)
{
    size_t count = sender_emit(sender, actions, capacity);

    if (count > 0U) {
        sender_arm(sender, now_ms);
    }
    return count;
}

size_t lab_sender_on_ack(LabSender *sender, uint32_t ack_seq,
                         uint64_t now_ms, LabAction *actions, size_t capacity)
{
    size_t count;

    if (ack_seq <= sender->base || ack_seq > sender->next) {
        return 0;
    }
    sender->base = ack_seq;
    sender->consecutive_timeouts = 0;
    if (sender->base == sender->next) {
        if (sender->base == sender->data_packets) {
            count = sender_emit(sender, actions, capacity);
            sender_arm(sender, now_ms);
            return count;
        }
        sender->timer_active = false;
        sender->complete = sender->base > sender->data_packets;
        return 0;
    }
    count = sender_emit(sender, actions, capacity);
    sender_arm(sender, now_ms);
    return count;
}

size_t lab_sender_on_timeout(LabSender *sender, uint64_t now_ms,
                             LabAction *actions, size_t capacity)
{
    size_t count = 0;

    if (!sender->timer_active) {
        return 0;
    }
    if (sender->consecutive_timeouts >= LAB_MAX_TIMEOUTS) {
        sender->failed = true;
        sender->timer_active = false;
        return 0;
    }
    for (uint32_t sequence = sender->base;
         sequence < sender->next && count < capacity; sequence++) {
        actions[count].type = LAB_ACTION_SEND;
        actions[count].packet = sender_packet(sender, sequence);
        count++;
    }
    sender->consecutive_timeouts++;
    sender_arm(sender, now_ms);
    return count;
}

void lab_receiver_init(LabReceiver *receiver)
{
    memset(receiver, 0, sizeof(*receiver));
}

size_t lab_receiver_on_packet(LabReceiver *receiver, const LabPacket *packet,
                              uint64_t now_ms, LabAction *actions,
                              size_t capacity)
{
    size_t count = 0;
    bool expected_fin;

    if (receiver->complete && packet->type == LAB_FIN &&
        packet->seq + 1U == receiver->expected && now_ms < receiver->linger_until_ms &&
        capacity > 0U) {
        actions[0].type = LAB_ACTION_SEND;
        memset(&actions[0].packet, 0, sizeof(actions[0].packet));
        actions[0].packet.type = LAB_ACK;
        actions[0].packet.seq = receiver->expected;
        return 1;
    }
    if (receiver->complete || receiver->failed || capacity == 0U) {
        return 0;
    }
    expected_fin = packet->type == LAB_FIN && packet->seq == receiver->expected;
    if (packet->type == LAB_DATA && packet->seq == receiver->expected) {
        actions[count].type = LAB_ACTION_DELIVER;
        actions[count].packet = *packet;
        count++;
        receiver->expected++;
    } else if (!expected_fin && packet->type != LAB_DATA) {
        return 0;
    } else if (!expected_fin) {
        /* A duplicate or out-of-order DATA still receives the cumulative ACK. */
    } else {
        receiver->expected++;
        receiver->complete = true;
        receiver->linger_until_ms = now_ms + LAB_RECEIVER_LINGER_MS;
    }
    if (count < capacity) {
        actions[count].type = LAB_ACTION_SEND;
        memset(&actions[count].packet, 0, sizeof(actions[count].packet));
        actions[count].packet.type = LAB_ACK;
        actions[count].packet.seq = receiver->expected;
        count++;
    }
    return count;
}

bool lab_receiver_expired(const LabReceiver *receiver, uint64_t now_ms)
{
    return receiver->complete && now_ms >= receiver->linger_until_ms;
}

static uint64_t wall_clock_ms(void)
{
    struct timespec value;

    // GCOVR_EXCL_START
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0;
    } // GCOVR_EXCL_STOP
    return (uint64_t) value.tv_sec * 1000U +
           (uint64_t) value.tv_nsec / 1000000U;
}

static int open_udp(const struct addrinfo *addresses,
                    const struct addrinfo **chosen)
{
    for (const struct addrinfo *address = addresses; address != NULL;
         address = address->ai_next) { // GCOVR_EXCL_LINE
        int socket_fd = socket(address->ai_family, address->ai_socktype,
                               address->ai_protocol);
        if (socket_fd >= 0) {
            *chosen = address;
            return socket_fd;
        }
    }
    return -1; // GCOVR_EXCL_LINE
}

static bool relay_hello(int socket_fd, const struct addrinfo *relay,
                        const char *message)
{
    uint8_t response[128];
    struct pollfd descriptor = {.fd = socket_fd, .events = POLLIN};

    for (unsigned int attempt = 0; attempt < LAB_HELLO_ATTEMPTS; attempt++) {
        ssize_t sent = sendto(socket_fd, message, strlen(message), 0,
                              relay->ai_addr, relay->ai_addrlen);
        // GCOVR_EXCL_START
        if (sent != (ssize_t) strlen(message)) {
            continue;
        } // GCOVR_EXCL_STOP
        // GCOVR_EXCL_START
        if (poll(&descriptor, 1, LAB_HELLO_TIMEOUT_MS) <= 0) {
            continue;
        } // GCOVR_EXCL_STOP
        ssize_t received = recvfrom(socket_fd, response, sizeof(response) - 1U,
                                    0, NULL, NULL);
        // GCOVR_EXCL_START
        if (received < 0) {
            continue;
        } // GCOVR_EXCL_STOP
        response[received] = '\0';
        if (strcmp((const char *) response, "OK") == 0) {
            return true;
        }
        if (strncmp((const char *) response, "ERR ", 4) == 0) {
            fprintf(stderr, "%s\n", response);
            return false;
        }
    }
    fprintf(stderr, "relay registration timed out\n"); // GCOVR_EXCL_LINE
    return false; // GCOVR_EXCL_LINE
}

static bool send_action(int socket_fd, const struct addrinfo *relay,
                        const LabAction *action)
{
    uint8_t encoded[LAB_MAX_PACKET];
    size_t length;

    (void) lab_encode_packet(&action->packet, encoded, sizeof(encoded), &length);
    return sendto(socket_fd, encoded, length, 0, relay->ai_addr,
                  relay->ai_addrlen) == (ssize_t) length;
}

static bool load_file(const char *name, uint8_t **data, size_t *size)
{
    FILE *file = fopen(name, "rb");
    long length;
    uint8_t *contents;

    // GCOVR_EXCL_START
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) {
            (void) fclose(file);
        }
        return false;
    } // GCOVR_EXCL_STOP
    length = ftell(file);
    // GCOVR_EXCL_START
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        (void) fclose(file);
        return false;
    } // GCOVR_EXCL_STOP
    contents = length == 0 ? NULL : malloc((size_t) length);
    // GCOVR_EXCL_START
    if (length > 0 && contents == NULL) {
        (void) fclose(file);
        return false;
    } // GCOVR_EXCL_STOP
    // GCOVR_EXCL_START
    if (length > 0 &&
        fread(contents, 1, (size_t) length, file) != (size_t) length) {
        free(contents);
        (void) fclose(file);
        return false;
    } // GCOVR_EXCL_STOP
    (void) fclose(file);
    *data = contents;
    *size = (size_t) length;
    return true;
}

static int run_sender(const LabTransferOptions *options,
                      const struct addrinfo *relay)
{
    const struct addrinfo *address;
    int socket_fd = open_udp(relay, &address);
    uint8_t *data = NULL;
    size_t size = 0;
    LabSender sender;
    LabAction actions[LAB_MAX_WINDOW + 1U];
    uint8_t received[LAB_MAX_PACKET];
    struct pollfd descriptor;

    // GCOVR_EXCL_START
    if (socket_fd < 0 || !load_file(options->file, &data, &size)) {
        free(data);
        if (socket_fd >= 0) {
            (void) close(socket_fd);
        }
        return 2;
    } // GCOVR_EXCL_STOP
    {
        char message[128];
        (void) snprintf(message, sizeof(message), "HELLO %s send %g %g %g",
                        options->session, options->loss, options->corrupt,
                        options->duplicate);
        // GCOVR_EXCL_START
        if (!relay_hello(socket_fd, address, message)) {
            free(data);
            (void) close(socket_fd);
            return 2;
        } // GCOVR_EXCL_STOP
    }
    lab_sender_init(&sender, data, size, options->window, options->timeout_ms);
    size_t count = lab_sender_start(&sender, wall_clock_ms(), actions,
                                    sizeof(actions) / sizeof(actions[0]));
    for (size_t i = 0; i < count; i++) {
        // GCOVR_EXCL_START
        if (!send_action(socket_fd, address, &actions[i])) {
            goto fail;
        } // GCOVR_EXCL_STOP
    }
    descriptor.fd = socket_fd;
    descriptor.events = POLLIN;
    while (!sender.complete && !sender.failed) {
        uint64_t current = wall_clock_ms();
        int timeout = sender.timer_active && sender.timer_due_ms > current ?
                      (int) (sender.timer_due_ms - current) : 0;
        int ready = poll(&descriptor, 1, timeout);
        // GCOVR_EXCL_START
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            goto fail;
        } // GCOVR_EXCL_STOP
        // GCOVR_EXCL_START
        if (ready == 0) {
            count = lab_sender_on_timeout(&sender, wall_clock_ms(), actions,
                                          sizeof(actions) / sizeof(actions[0]));
        } // GCOVR_EXCL_STOP
        else {
            ssize_t length = recvfrom(socket_fd, received, sizeof(received),
                                      0, NULL, NULL);
            LabPacket packet;
            if (length < 0 || // GCOVR_EXCL_LINE
                !lab_decode_packet(received, (size_t) length, &packet) ||
                packet.type != LAB_ACK) {
                continue;
            }
            count = lab_sender_on_ack(&sender, packet.seq, wall_clock_ms(),
                                      actions, sizeof(actions) / sizeof(actions[0]));
        }
        for (size_t i = 0; i < count; i++) {
            // GCOVR_EXCL_START
            if (!send_action(socket_fd, address, &actions[i])) {
                goto fail;
            } // GCOVR_EXCL_STOP
        }
    }
    free(data);
    (void) close(socket_fd);
    return sender.complete ? 0 : 2;

// GCOVR_EXCL_START
fail:
    free(data);
    (void) close(socket_fd);
    return 2;
// GCOVR_EXCL_STOP
}

static int run_receiver(const LabTransferOptions *options,
                        const struct addrinfo *relay)
{
    const struct addrinfo *address;
    int socket_fd = open_udp(relay, &address);
    FILE *file;
    LabReceiver receiver;
    LabAction actions[2];
    uint8_t buffer[LAB_MAX_PACKET];
    struct pollfd descriptor;
    uint64_t last = wall_clock_ms();

    // GCOVR_EXCL_START
    if (socket_fd < 0) {
        return 2;
    } // GCOVR_EXCL_STOP
    file = fopen(options->file, "wb");
    // GCOVR_EXCL_START
    if (file == NULL) {
        (void) close(socket_fd);
        return 2;
    } // GCOVR_EXCL_STOP
    {
        char message[64];
        (void) snprintf(message, sizeof(message), "HELLO %s recv",
                        options->session);
        // GCOVR_EXCL_START
        if (!relay_hello(socket_fd, address, message)) {
            goto fail;
        } // GCOVR_EXCL_STOP
    }
    lab_receiver_init(&receiver);
    descriptor.fd = socket_fd;
    descriptor.events = POLLIN;
    while (!lab_receiver_expired(&receiver, wall_clock_ms()) &&
           wall_clock_ms() - last < LAB_RECEIVER_IDLE_MS) {
        uint64_t current = wall_clock_ms();
        int timeout = (int) (LAB_RECEIVER_IDLE_MS - (current - last));
        if (receiver.complete && receiver.linger_until_ms > current &&
            receiver.linger_until_ms - current < (uint64_t) timeout) {
            timeout = (int) (receiver.linger_until_ms - current);
        }
        int ready = poll(&descriptor, 1, timeout);
        // GCOVR_EXCL_START
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            goto fail;
        } // GCOVR_EXCL_STOP
        // GCOVR_EXCL_START
        if (ready == 0) {
            if (receiver.complete &&
                wall_clock_ms() >= receiver.linger_until_ms) {
                break;
            }
            goto fail;
        } // GCOVR_EXCL_STOP
        ssize_t length = recvfrom(socket_fd, buffer, sizeof(buffer), 0, NULL, NULL);
        LabPacket packet;
        if (length < 0 || // GCOVR_EXCL_LINE
            !lab_decode_packet(buffer, (size_t) length, &packet)) {
            continue;
        }
        last = wall_clock_ms();
        size_t count = lab_receiver_on_packet(&receiver, &packet, last,
                                              actions, 2);
        for (size_t i = 0; i < count; i++) {
            // GCOVR_EXCL_START
            if (actions[i].type == LAB_ACTION_DELIVER &&
                fwrite(actions[i].packet.payload, 1, actions[i].packet.length,
                       file) != actions[i].packet.length) {
                goto fail;
            } // GCOVR_EXCL_STOP
            // GCOVR_EXCL_START
            if (actions[i].type == LAB_ACTION_SEND &&
                !send_action(socket_fd, address, &actions[i])) {
                goto fail;
            } // GCOVR_EXCL_STOP
        }
    }
    // GCOVR_EXCL_START
    if (!receiver.complete) {
        goto fail;
    } // GCOVR_EXCL_STOP
    (void) fclose(file);
    (void) close(socket_fd);
    return 0;

// GCOVR_EXCL_START
fail:
    (void) fclose(file);
    (void) close(socket_fd);
    return 2;
// GCOVR_EXCL_STOP
}

int lab_run_transfer(const LabTransferOptions *options)
{
    struct addrinfo hints = {0};
    struct addrinfo *relay = NULL;
    char port[sizeof("65535")];
    int error;
    int status;

    if (options == NULL || options->session == NULL ||
        options->relay == NULL || options->file == NULL) {
        return 2;
    }
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    (void) snprintf(port, sizeof(port), "%u", options->port);
    error = getaddrinfo(options->relay, port, &hints, &relay);
    // GCOVR_EXCL_START
    if (error != 0) {
        fprintf(stderr, "cannot resolve relay %s: %s\n",
                options->relay, gai_strerror(error));
        return 2;
    } // GCOVR_EXCL_STOP
    status = options->mode == LAB_SEND_MODE ? run_sender(options, relay) :
                                               run_receiver(options, relay);
    freeaddrinfo(relay);
    return status;
}
