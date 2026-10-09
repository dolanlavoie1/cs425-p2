#ifndef LAB_H
#define LAB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LAB_MAX_PAYLOAD 1024U
#define LAB_PACKET_HEADER 10U
#define LAB_MAX_PACKET (LAB_PACKET_HEADER + LAB_MAX_PAYLOAD)
#define LAB_MAX_WINDOW 64U
#define LAB_MAX_TIMEOUTS 10U
#define LAB_RECEIVER_LINGER_MS 2000U
#define LAB_DEFAULT_PORT 4250U
#define LAB_DEFAULT_TIMEOUT_MS 250U
#define LAB_DEFAULT_WINDOW 8U
#define LAB_MAX_SESSION_LENGTH 32U
#define LAB_MAX_TIMEOUT_MS 0xffffffffU
#define LAB_HELLO_ATTEMPTS 5U
#define LAB_HELLO_TIMEOUT_MS 1000
#define LAB_RECEIVER_IDLE_MS 30000U

typedef enum {
    LAB_DATA = 0,
    LAB_ACK = 1,
    LAB_FIN = 2
} LabPacketType;

typedef struct {
    LabPacketType type;
    uint32_t seq;
    uint16_t length;
    uint8_t payload[LAB_MAX_PAYLOAD];
} LabPacket;

typedef enum {
    LAB_ACTION_SEND,
    LAB_ACTION_DELIVER
} LabActionType;

typedef struct {
    LabActionType type;
    LabPacket packet;
} LabAction;

typedef struct {
    const uint8_t *data;
    size_t size;
    uint32_t data_packets;
    uint32_t base;
    uint32_t next;
    uint32_t window;
    uint32_t timeout_ms;
    uint32_t consecutive_timeouts;
    uint64_t timer_due_ms;
    bool timer_active;
    bool failed;
    bool complete;
} LabSender;

typedef struct {
    uint32_t expected;
    bool complete;
    bool failed;
    uint64_t linger_until_ms;
} LabReceiver;

typedef enum {
    LAB_SEND_MODE,
    LAB_RECEIVE_MODE
} LabTransferMode;

typedef struct {
    LabTransferMode mode;
    const char *session;
    const char *relay;
    const char *file;
    uint32_t window;
    uint32_t timeout_ms;
    uint16_t port;
    double loss;
    double corrupt;
    double duplicate;
} LabTransferOptions;

uint16_t lab_checksum(const uint8_t *data, size_t length);
bool lab_encode_packet(const LabPacket *packet, uint8_t *buffer,
                       size_t capacity, size_t *encoded_length);
bool lab_decode_packet(const uint8_t *buffer, size_t length, LabPacket *packet);

void lab_sender_init(LabSender *sender, const uint8_t *data, size_t size,
                     uint32_t window, uint32_t timeout_ms);
size_t lab_sender_start(LabSender *sender, uint64_t now_ms,
                        LabAction *actions, size_t capacity);
size_t lab_sender_on_ack(LabSender *sender, uint32_t ack_seq,
                         uint64_t now_ms, LabAction *actions, size_t capacity);
size_t lab_sender_on_timeout(LabSender *sender, uint64_t now_ms,
                             LabAction *actions, size_t capacity);

void lab_receiver_init(LabReceiver *receiver);
size_t lab_receiver_on_packet(LabReceiver *receiver, const LabPacket *packet,
                              uint64_t now_ms, LabAction *actions,
                              size_t capacity);
bool lab_receiver_expired(const LabReceiver *receiver, uint64_t now_ms);
int lab_run_transfer(const LabTransferOptions *options);

#endif
