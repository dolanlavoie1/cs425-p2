#include "harness/unity.h"
#include "../src/lab.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

void setUp(void) {}
void tearDown(void) {}

static LabPacket data_packet(uint32_t seq, const char *text)
{
    LabPacket packet = {0};
    packet.type = LAB_DATA;
    packet.seq = seq;
    packet.length = (uint16_t) strlen(text);
    memcpy(packet.payload, text, packet.length);
    return packet;
}

static LabPacket ack_packet(uint32_t seq)
{
    LabPacket packet = {0};
    packet.type = LAB_ACK;
    packet.seq = seq;
    return packet;
}

void test_checksum_rfc_and_odd_length(void)
{
    const uint8_t rfc[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    const uint8_t odd[] = {0x12, 0x34, 0x56};
    const uint8_t carry[] = {0xff, 0xff, 0xff};
    TEST_ASSERT_EQUAL_HEX16(0x220d, lab_checksum(rfc, sizeof(rfc)));
    TEST_ASSERT_EQUAL_HEX16(0x97cb, lab_checksum(odd, sizeof(odd)));
    TEST_ASSERT_EQUAL_HEX16(0x00ff, lab_checksum(carry, sizeof(carry)));
}

void test_packet_round_trip_and_corruption(void)
{
    LabPacket input = data_packet(7, "Hi!");
    LabPacket output;
    uint8_t wire[LAB_MAX_PACKET];
    size_t length = 0;

    TEST_ASSERT_TRUE(lab_encode_packet(&input, wire, sizeof(wire), &length));
    TEST_ASSERT_EQUAL_UINT(13, length);
    TEST_ASSERT_TRUE(lab_decode_packet(wire, length, &output));
    TEST_ASSERT_EQUAL_UINT32(input.seq, output.seq);
    TEST_ASSERT_EQUAL_UINT(input.length, output.length);
    TEST_ASSERT_EQUAL_MEMORY(input.payload, output.payload, input.length);
    wire[12] ^= 1U;
    TEST_ASSERT_FALSE(lab_decode_packet(wire, length, &output));
}

void test_packet_validation_failures(void)
{
    LabPacket input = data_packet(1, "x");
    LabPacket output;
    uint8_t wire[LAB_MAX_PACKET];
    size_t length;

    TEST_ASSERT_TRUE(lab_encode_packet(&input, wire, sizeof(wire), &length));
    TEST_ASSERT_FALSE(lab_decode_packet(wire, LAB_PACKET_HEADER - 1U, &output));
    wire[9] = 2;
    TEST_ASSERT_FALSE(lab_decode_packet(wire, length, &output));
    wire[9] = 1;
    wire[0] = 9;
    TEST_ASSERT_FALSE(lab_decode_packet(wire, length, &output));
    TEST_ASSERT_FALSE(lab_encode_packet(&input, wire, LAB_PACKET_HEADER, &length));
    TEST_ASSERT_FALSE(lab_encode_packet(NULL, wire, sizeof(wire), &length));
    input.type = LAB_ACK;
    input.length = 1;
    TEST_ASSERT_FALSE(lab_encode_packet(&input, wire, sizeof(wire), &length));
    input = data_packet(1, "x");
    TEST_ASSERT_TRUE(lab_encode_packet(&input, wire, sizeof(wire), &length));
    wire[0] = LAB_ACK;
    wire[2] = 0;
    wire[3] = 0;
    {
        uint16_t checksum = lab_checksum(wire, length);
        wire[2] = (uint8_t) (checksum >> 8);
        wire[3] = (uint8_t) checksum;
    }
    TEST_ASSERT_FALSE(lab_decode_packet(wire, length, &output));
}

void test_receiver_order_duplicates_and_fin_linger(void)
{
    LabReceiver receiver;
    LabAction actions[2];
    LabPacket packet = data_packet(1, "late");
    LabPacket invalid = ack_packet(0);
    LabPacket fin = { .type = LAB_FIN, .seq = 1 };
    size_t count;

    lab_receiver_init(&receiver);
    count = lab_receiver_on_packet(&receiver, &packet, 0, actions, 2);
    TEST_ASSERT_EQUAL_UINT(1, count);
    TEST_ASSERT_EQUAL_UINT(LAB_ACK, actions[0].packet.type);
    TEST_ASSERT_EQUAL_UINT32(0, actions[0].packet.seq);
    TEST_ASSERT_EQUAL_UINT(0, lab_receiver_on_packet(&receiver, &invalid, 0,
                                                     actions, 2));
    packet = data_packet(0, "ok");
    count = lab_receiver_on_packet(&receiver, &packet, 1, actions, 2);
    TEST_ASSERT_EQUAL_UINT(2, count);
    TEST_ASSERT_EQUAL_UINT(LAB_ACTION_DELIVER, actions[0].type);
    TEST_ASSERT_EQUAL_UINT(LAB_ACTION_SEND, actions[1].type);
    count = lab_receiver_on_packet(&receiver, &packet, 2, actions, 2);
    TEST_ASSERT_EQUAL_UINT(1, count);
    TEST_ASSERT_EQUAL_UINT32(1, actions[0].packet.seq);
    count = lab_receiver_on_packet(&receiver, &fin, 3, actions, 2);
    TEST_ASSERT_EQUAL_UINT(1, count);
    TEST_ASSERT_TRUE(receiver.complete);
    TEST_ASSERT_FALSE(lab_receiver_expired(&receiver, 2002));
    fin.seq = 1;
    count = lab_receiver_on_packet(&receiver, &fin, 100, actions, 2);
    TEST_ASSERT_EQUAL_UINT(1, count);
    TEST_ASSERT_EQUAL_UINT32(2, actions[0].packet.seq);
    TEST_ASSERT_TRUE(lab_receiver_expired(&receiver, 2003));
    TEST_ASSERT_EQUAL_UINT(0, lab_receiver_on_packet(&receiver, &packet, 2004,
                                                     actions, 2));
}

void test_sender_window_cumulative_ack_and_duplicate(void)
{
    const uint8_t data[2048] = {0};
    LabSender sender;
    LabAction actions[65];
    size_t count;

    lab_sender_init(&sender, data, sizeof(data), 2, 50);
    count = lab_sender_start(&sender, 0, actions, 65);
    TEST_ASSERT_EQUAL_UINT(2, count);
    TEST_ASSERT_EQUAL_UINT32(2, sender.next);
    TEST_ASSERT_EQUAL_UINT(0, lab_sender_on_ack(&sender, 0, 1, actions, 65));
    count = lab_sender_on_ack(&sender, 2, 2, actions, 65);
    TEST_ASSERT_EQUAL_UINT(1, count);
    TEST_ASSERT_EQUAL_UINT(LAB_FIN, actions[0].packet.type);
    TEST_ASSERT_EQUAL_UINT32(2, actions[0].packet.seq);
    TEST_ASSERT_EQUAL_UINT(0, lab_sender_on_ack(&sender, 2, 3, actions, 65));
    sender.complete = true;
    sender.timer_active = false;
    TEST_ASSERT_EQUAL_UINT(0, lab_sender_on_timeout(&sender, 4, actions, 65));
}

void test_sender_stop_and_wait_advances_after_each_ack(void)
{
    const uint8_t data[] = "two packets";
    LabSender sender;
    LabAction actions[2];

    lab_sender_init(&sender, data, sizeof(data) - 1U, 1, 50);
    TEST_ASSERT_EQUAL_UINT(1, lab_sender_start(&sender, 0, actions, 2));
    TEST_ASSERT_EQUAL_UINT32(1, sender.next);
    TEST_ASSERT_EQUAL_UINT(1, lab_sender_on_ack(&sender, 1, 1,
                                                actions, 2));
    TEST_ASSERT_EQUAL_UINT32(2, sender.next);
    TEST_ASSERT_TRUE(sender.timer_active);
}

void test_sender_completes_after_fin_ack(void)
{
    LabSender sender;
    LabAction actions[2];

    lab_sender_init(&sender, NULL, 0, 1, 50);
    TEST_ASSERT_EQUAL_UINT(1, lab_sender_start(&sender, 0, actions, 2));
    TEST_ASSERT_EQUAL_UINT(LAB_FIN, actions[0].packet.type);
    TEST_ASSERT_EQUAL_UINT(0, lab_sender_on_ack(&sender, 1, 1, actions, 2));
    TEST_ASSERT_TRUE(sender.complete);
    TEST_ASSERT_FALSE(sender.timer_active);
}

void test_sender_timeout_retries_and_gives_up(void)
{
    const uint8_t data[] = "abc";
    LabSender sender;
    LabAction actions[65];
    size_t count;

    lab_sender_init(&sender, data, sizeof(data) - 1U, 4, 10);
    (void) lab_sender_start(&sender, 0, actions, 65);
    count = lab_sender_on_timeout(&sender, 10, actions, 65);
    TEST_ASSERT_EQUAL_UINT(1, count);
    TEST_ASSERT_EQUAL_UINT(1, sender.consecutive_timeouts);
    for (unsigned int i = 1; i < LAB_MAX_TIMEOUTS; i++) {
        (void) lab_sender_on_timeout(&sender, 10U * (i + 1U), actions, 65);
    }
    TEST_ASSERT_EQUAL_UINT(0, lab_sender_on_timeout(&sender, 200, actions, 65));
    TEST_ASSERT_TRUE(sender.failed);
    sender.timer_active = false;
    TEST_ASSERT_EQUAL_UINT(0, lab_sender_on_timeout(&sender, 300, actions, 65));
}

void test_empty_and_exact_multiple_packetization(void)
{
    LabSender empty;
    LabSender exact;
    LabAction actions[65];
    uint8_t data[2048] = {0};

    lab_sender_init(&empty, NULL, 0, 1, 10);
    TEST_ASSERT_EQUAL_UINT(1, lab_sender_start(&empty, 0, actions, 65));
    TEST_ASSERT_EQUAL_UINT(LAB_FIN, actions[0].packet.type);
    lab_sender_init(&exact, data, sizeof(data), 64, 10);
    TEST_ASSERT_EQUAL_UINT(2, exact.data_packets);
    TEST_ASSERT_EQUAL_UINT(2, lab_sender_start(&exact, 0, actions, 65));
    TEST_ASSERT_EQUAL_UINT(LAB_DATA, actions[1].packet.type);
    TEST_ASSERT_EQUAL_UINT(LAB_MAX_PAYLOAD, actions[1].packet.length);
}

static void queue_action(LabPacket *queue, size_t *length,
                         const LabAction *action, unsigned int *random_state)
{
    unsigned int roll = rand_r(random_state) % 10U;
    uint8_t wire[LAB_MAX_PACKET];
    size_t wire_length;
    LabPacket packet;

    if (roll < 2U) return;
    TEST_ASSERT_TRUE(lab_encode_packet(&action->packet, wire, sizeof(wire),
                                       &wire_length));
    if (roll == 2U && wire_length > LAB_PACKET_HEADER) {
        wire[LAB_PACKET_HEADER] ^= 1U;
    }
    if (!lab_decode_packet(wire, wire_length, &packet)) return;
    queue[(*length)++] = packet;
    if (roll == 3U) queue[(*length)++] = packet;
}

void test_complete_seeded_lossy_transfer(void)
{
    uint8_t source[5000];
    uint8_t delivered[5000];
    LabPacket to_receiver[256];
    LabPacket to_sender[256];
    LabAction actions[65];
    LabSender sender;
    LabReceiver receiver;
    size_t to_receiver_length = 0;
    size_t to_sender_length = 0;
    size_t delivered_length = 0;
    unsigned int random_state = 7;
    uint64_t now = 0;

    for (size_t i = 0; i < sizeof(source); i++) source[i] = (uint8_t) i;
    lab_sender_init(&sender, source, sizeof(source), 8, 20);
    lab_receiver_init(&receiver);
    size_t count = lab_sender_start(&sender, now, actions, 65);
    for (size_t i = 0; i < count; i++) {
        queue_action(to_receiver, &to_receiver_length, &actions[i], &random_state);
    }
    while (!sender.complete && !sender.failed && now < 100000U) {
        while (to_receiver_length > 0U) {
            LabPacket packet = to_receiver[--to_receiver_length];
            count = lab_receiver_on_packet(&receiver, &packet, now, actions, 2);
            for (size_t i = 0; i < count; i++) {
                if (actions[i].type == LAB_ACTION_DELIVER) {
                    memcpy(delivered + delivered_length, actions[i].packet.payload,
                           actions[i].packet.length);
                    delivered_length += actions[i].packet.length;
                } else {
                    queue_action(to_sender, &to_sender_length, &actions[i],
                                 &random_state);
                }
            }
        }
        while (to_sender_length > 0U) {
            LabPacket packet = to_sender[--to_sender_length];
            if (packet.type == LAB_ACK) {
                count = lab_sender_on_ack(&sender, packet.seq, now, actions, 65);
                for (size_t i = 0; i < count; i++) {
                    queue_action(to_receiver, &to_receiver_length, &actions[i],
                                 &random_state);
                }
            }
        }
        if (sender.timer_active && now >= sender.timer_due_ms) {
            count = lab_sender_on_timeout(&sender, now, actions, 65);
            for (size_t i = 0; i < count; i++) {
                queue_action(to_receiver, &to_receiver_length, &actions[i],
                             &random_state);
            }
        } else {
            now++;
        }
    }
    TEST_ASSERT_TRUE(sender.complete);
    TEST_ASSERT_FALSE(sender.failed);
    TEST_ASSERT_EQUAL_UINT(sizeof(source), delivered_length);
    TEST_ASSERT_EQUAL_MEMORY(source, delivered, sizeof(source));
}

void test_transfer_rejects_missing_options(void)
{
    TEST_ASSERT_EQUAL_INT(2, lab_run_transfer(NULL));
}

static int relay_socket(uint16_t *port)
{
    struct sockaddr_in address = {0};
    socklen_t length = sizeof(address);
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);

    TEST_ASSERT_TRUE(socket_fd >= 0);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    TEST_ASSERT_EQUAL_INT(0, bind(socket_fd, (struct sockaddr *) &address,
                                  sizeof(address)));
    TEST_ASSERT_EQUAL_INT(0, getsockname(socket_fd, (struct sockaddr *) &address,
                                         &length));
    *port = ntohs(address.sin_port);
    return socket_fd;
}

static void test_relay_sender(int socket_fd)
{
    struct sockaddr_storage peer;
    socklen_t peer_length = sizeof(peer);
    uint8_t buffer[LAB_MAX_PACKET];
    uint8_t encoded[LAB_MAX_PACKET];
    LabPacket packet;
    LabPacket ack = {0};
    ssize_t length = recvfrom(socket_fd, buffer, sizeof(buffer), 0,
                              (struct sockaddr *) &peer, &peer_length);

    if (length > 0 && (size_t) length < sizeof(buffer) &&
        memcmp(buffer, "HELLO ", 6) == 0) {
        (void) sendto(socket_fd, "OK", 2, 0, (struct sockaddr *) &peer,
                      peer_length);
        (void) sendto(socket_fd, "bad", 3, 0, (struct sockaddr *) &peer,
                      peer_length);
    }
    for (;;) {
        length = recvfrom(socket_fd, buffer, sizeof(buffer), 0,
                          (struct sockaddr *) &peer, &peer_length);
        if (length <= 0 || !lab_decode_packet(buffer, (size_t) length, &packet)) {
            _exit(1);
        }
        ack.type = LAB_ACK;
        ack.seq = packet.seq + 1U;
        size_t encoded_length = 0;
        if (!lab_encode_packet(&ack, encoded, sizeof(encoded), &encoded_length)) {
            _exit(1);
        }
        if (sendto(socket_fd, encoded, encoded_length, 0,
                   (struct sockaddr *) &peer, peer_length) < 0) {
            _exit(1);
        }
        if (packet.type == LAB_FIN) {
            _exit(0);
        }
    }
}

static void test_relay_receiver(int socket_fd, const uint8_t *payload,
                                size_t payload_length)
{
    struct sockaddr_storage peer;
    socklen_t peer_length = sizeof(peer);
    uint8_t buffer[LAB_MAX_PACKET];
    uint8_t encoded[LAB_MAX_PACKET];
    LabPacket packet;
    ssize_t length = recvfrom(socket_fd, buffer, sizeof(buffer), 0,
                              (struct sockaddr *) &peer, &peer_length);

    if (length <= 0 || (size_t) length >= sizeof(buffer) ||
        memcmp(buffer, "HELLO ", 6) != 0 ||
        sendto(socket_fd, "OK", 2, 0, (struct sockaddr *) &peer,
               peer_length) < 0) {
        _exit(1);
    }
    (void) sendto(socket_fd, "bad", 3, 0, (struct sockaddr *) &peer,
                  peer_length);
    packet.type = LAB_DATA;
    packet.seq = 0;
    packet.length = (uint16_t) payload_length;
    memcpy(packet.payload, payload, payload_length);
    size_t encoded_length = 0;
    if (!lab_encode_packet(&packet, encoded, sizeof(encoded), &encoded_length) ||
        sendto(socket_fd, encoded, encoded_length, 0,
               (struct sockaddr *) &peer, peer_length) < 0) {
        _exit(1);
    }
    packet.type = LAB_FIN;
    packet.seq = 1;
    packet.length = 0;
    if (!lab_encode_packet(&packet, encoded, sizeof(encoded), &encoded_length) ||
        sendto(socket_fd, encoded, encoded_length, 0,
               (struct sockaddr *) &peer, peer_length) < 0) {
        _exit(1);
    }
    for (unsigned int i = 0; i < 2; i++) {
        length = recvfrom(socket_fd, buffer, sizeof(buffer), 0,
                          (struct sockaddr *) &peer, &peer_length);
        if (length <= 0 || !lab_decode_packet(buffer, (size_t) length, &packet) ||
            packet.type != LAB_ACK) {
            _exit(1);
        }
    }
    _exit(0);
}

static void test_relay_error(int socket_fd)
{
    struct sockaddr_storage peer;
    socklen_t peer_length = sizeof(peer);
    char buffer[128];

    if (recvfrom(socket_fd, buffer, sizeof(buffer), 0,
                 (struct sockaddr *) &peer, &peer_length) > 0) {
        (void) sendto(socket_fd, "ERR rejected", 12, 0,
                      (struct sockaddr *) &peer, peer_length);
    }
    _exit(0);
}

static pid_t start_test_relay(bool sender, uint16_t *port,
                              const uint8_t *payload, size_t payload_length)
{
    int socket_fd = relay_socket(port);
    pid_t child;

    child = fork();
    TEST_ASSERT_TRUE(child >= 0);
    if (child == 0) {
        if (sender) {
            test_relay_sender(socket_fd);
        } else {
            test_relay_receiver(socket_fd, payload, payload_length);
        }
    }
    close(socket_fd);
    return child;
}

void test_udp_transfer_orchestration(void)
{
    const uint8_t source[] = "orchestrated transfer";
    char input_name[] = "/tmp/lab-input-XXXXXX";
    char output_name[] = "/tmp/lab-output-XXXXXX";
    LabTransferOptions options = {
        .mode = LAB_SEND_MODE, .session = "test", .relay = "127.0.0.1",
        .file = input_name, .window = 4, .timeout_ms = 100, .port = 0
    };
    uint16_t port;
    int input = mkstemp(input_name);
    int output = mkstemp(output_name);
    ssize_t written;
    pid_t child;
    int status;

    TEST_ASSERT_TRUE(input >= 0);
    TEST_ASSERT_TRUE(output >= 0);
    written = write(input, source, sizeof(source) - 1U);
    TEST_ASSERT_EQUAL_INT((int) (sizeof(source) - 1U), (int) written);
    close(input);
    close(output);

    {
        int socket_fd = relay_socket(&port);
        child = fork();
        TEST_ASSERT_TRUE(child >= 0);
        if (child == 0) {
            test_relay_error(socket_fd);
        }
        close(socket_fd);
        options.port = port;
        TEST_ASSERT_EQUAL_INT(2, lab_run_transfer(&options));
        TEST_ASSERT_EQUAL_INT((int) child, (int) waitpid(child, &status, 0));
    }

    child = start_test_relay(true, &port, NULL, 0);
    options.port = port;
    TEST_ASSERT_EQUAL_INT(0, lab_run_transfer(&options));
    TEST_ASSERT_EQUAL_INT((int) child, (int) waitpid(child, &status, 0));
    TEST_ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    options.mode = LAB_RECEIVE_MODE;
    options.file = output_name;
    child = start_test_relay(false, &port, source, sizeof(source) - 1U);
    options.port = port;
    TEST_ASSERT_EQUAL_INT(0, lab_run_transfer(&options));
    TEST_ASSERT_EQUAL_INT((int) child, (int) waitpid(child, &status, 0));
    TEST_ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    {
        char received[sizeof(source)] = {0};
        FILE *file = fopen(output_name, "rb");
        TEST_ASSERT_NOT_NULL(file);
        TEST_ASSERT_EQUAL_UINT(sizeof(source) - 1U,
                               fread(received, 1, sizeof(source) - 1U, file));
        TEST_ASSERT_EQUAL_MEMORY(source, received, sizeof(source) - 1U);
        fclose(file);
    }
    unlink(input_name);
    unlink(output_name);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_checksum_rfc_and_odd_length);
    RUN_TEST(test_packet_round_trip_and_corruption);
    RUN_TEST(test_packet_validation_failures);
    RUN_TEST(test_receiver_order_duplicates_and_fin_linger);
    RUN_TEST(test_sender_window_cumulative_ack_and_duplicate);
    RUN_TEST(test_sender_stop_and_wait_advances_after_each_ack);
    RUN_TEST(test_sender_completes_after_fin_ack);
    RUN_TEST(test_sender_timeout_retries_and_gives_up);
    RUN_TEST(test_empty_and_exact_multiple_packetization);
    RUN_TEST(test_complete_seeded_lossy_transfer);
    RUN_TEST(test_transfer_rejects_missing_options);
    RUN_TEST(test_udp_transfer_orchestration);
    return UNITY_END();
}
