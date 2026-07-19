#ifndef CT_PROTOCOL_H
#define CT_PROTOCOL_H
#include "ctunnel.h"
#include <stddef.h>
#include <stdint.h>
#define CT_MAGIC 0x4354554eU
#define CT_PROTOCOL_VERSION 2
#define CT_FLAG_ENCRYPTED 1
typedef enum {
    CT_MSG_CLIENT_HELLO = 1,
    CT_MSG_SERVER_HELLO,
    CT_MSG_CLIENT_AUTH,
    CT_MSG_AUTH_OK,
    CT_MSG_AUTH_FAILED,
    CT_MSG_REGISTER_SERVICE = 10,
    CT_MSG_REGISTER_OK,
    CT_MSG_REGISTER_FAILED,
    CT_MSG_UNREGISTER_SERVICE,
    CT_MSG_REQUEST_WORK_CONNECTION = 20,
    CT_MSG_WORK_CONNECTION_BIND,
    CT_MSG_START_STREAM,
    CT_MSG_STREAM_READY,
    CT_MSG_STREAM_FAILED,
    CT_MSG_CLOSE_STREAM,
    CT_MSG_PING = 30,
    CT_MSG_PONG,
    CT_MSG_ERROR,
    CT_MSG_GOAWAY
} ct_msg_type;
typedef struct {
    uint8_t type, flags;
    uint32_t payload_len;
    uint64_t session_id, stream_id, sequence;
} ct_frame_header;
int ct_frame_header_encode(const ct_frame_header *, uint8_t out[CT_FRAME_HEADER_SIZE]);
int ct_frame_header_decode(const uint8_t in[CT_FRAME_HEADER_SIZE], ct_frame_header *);
int ct_message_type_valid(uint8_t);
void ct_put_u16(uint8_t *, uint16_t);
void ct_put_u32(uint8_t *, uint32_t);
void ct_put_u64(uint8_t *, uint64_t);
uint16_t ct_get_u16(const uint8_t *);
uint32_t ct_get_u32(const uint8_t *);
uint64_t ct_get_u64(const uint8_t *);
int ct_pack_string(uint8_t *, size_t, size_t *, const char *, size_t);
int ct_unpack_string(const uint8_t *, size_t, size_t *, char *, size_t);
#endif
