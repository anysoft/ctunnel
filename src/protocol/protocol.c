#include "protocol/protocol.h"
#include "protocol/diagnostics.h"
#include <string.h>
void ct_put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
void ct_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
void ct_put_u64(uint8_t *p, uint64_t v) {
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}
uint16_t ct_get_u16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
uint32_t ct_get_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
uint64_t ct_get_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}
int ct_message_type_valid(uint8_t t) {
    switch (t) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 10:
    case 11:
    case 12:
    case 13:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
    case 25:
    case 30:
    case 31:
    case 32:
    case 33:
        return 1;
    default:
        return 0;
    }
}
int ct_frame_header_encode(const ct_frame_header *h, uint8_t *p) {
    if (!ct_message_type_valid(h->type) || h->payload_len > CT_MAX_FRAME_PAYLOAD) {
        ct_protocol_diagnostic("invalid outgoing frame header");
        return -1;
    }
    ct_put_u32(p, CT_MAGIC);
    p[4] = CT_PROTOCOL_VERSION;
    p[5] = h->type;
    p[6] = h->flags;
    p[7] = CT_FRAME_HEADER_SIZE;
    ct_put_u32(p + 8, h->payload_len);
    ct_put_u64(p + 12, h->session_id);
    ct_put_u64(p + 20, h->stream_id);
    ct_put_u64(p + 28, h->sequence);
    return 0;
}
int ct_frame_header_decode(const uint8_t *p, ct_frame_header *h) {
    if (ct_get_u32(p) != CT_MAGIC || p[4] != CT_PROTOCOL_VERSION || p[7] != CT_FRAME_HEADER_SIZE ||
        !ct_message_type_valid(p[5])) {
        ct_protocol_diagnostic("invalid incoming frame identity or type");
        return -1;
    }
    h->type = p[5];
    h->flags = p[6];
    h->payload_len = ct_get_u32(p + 8);
    h->session_id = ct_get_u64(p + 12);
    h->stream_id = ct_get_u64(p + 20);
    h->sequence = ct_get_u64(p + 28);
    if (h->payload_len > CT_MAX_FRAME_PAYLOAD) {
        ct_protocol_diagnostic("incoming frame exceeds compiled payload limit");
        return -1;
    }
    return 0;
}
int ct_pack_string(uint8_t *b, size_t cap, size_t *off, const char *s, size_t max) {
    size_t n = strlen(s);
    if (n > max || n > 65535 || *off > cap || cap - *off < 2 + n)
        return -1;
    ct_put_u16(b + *off, (uint16_t)n);
    memcpy(b + *off + 2, s, n);
    *off += 2 + n;
    return 0;
}
int ct_unpack_string(const uint8_t *b, size_t n, size_t *off, char *out, size_t cap) {
    if (*off > n || n - *off < 2)
        return -1;
    size_t z = ct_get_u16(b + *off);
    if (z >= cap || n - *off - 2 < z)
        return -1;
    memcpy(out, b + *off + 2, z);
    out[z] = 0;
    *off += 2 + z;
    return 0;
}
int ct_data_record_header_decode(const uint8_t header[12], size_t max_plaintext,
                                 uint64_t previous_sequence, uint32_t *encoded_length,
                                 uint64_t *sequence) {
    uint32_t length = ct_get_u32(header);
    uint64_t received_sequence = ct_get_u64(header + 4);
    if (max_plaintext > UINT32_MAX - CT_RECORD_TAG_SIZE || length < CT_RECORD_TAG_SIZE ||
        length > max_plaintext + CT_RECORD_TAG_SIZE || previous_sequence == UINT64_MAX ||
        received_sequence != previous_sequence + 1)
        return -1;
    *encoded_length = length;
    *sequence = received_sequence;
    return 0;
}

int ct_register_request_decode(const uint8_t *payload, size_t length, char *service_id,
                               size_t service_capacity, char *remote_address,
                               size_t address_capacity, uint16_t *remote_port, uint8_t *type,
                               uint8_t *mode) {
    size_t offset = 0;
    if (ct_unpack_string(payload, length, &offset, service_id, service_capacity) ||
        ct_unpack_string(payload, length, &offset, remote_address, address_capacity) ||
        length - offset != 4)
        return -1;
    *remote_port = ct_get_u16(payload + offset);
    *type = payload[offset + 2];
    *mode = payload[offset + 3];
    return 0;
}
