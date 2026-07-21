#include "net/proxy_protocol.h"
#include "protocol/protocol.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

static int is_v4_mapped(const ct_endpoint *endpoint) {
    static const uint8_t prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    return endpoint->family == 6 && !memcmp(endpoint->addr, prefix, sizeof prefix);
}

static int normalize_pair(ct_endpoint *source, ct_endpoint *destination,
                          const ct_stream_metadata *metadata) {
    *source = metadata->source;
    *destination = metadata->destination;
    if (source->family == 6 && destination->family == 6 && is_v4_mapped(source) &&
        is_v4_mapped(destination)) {
        memmove(source->addr, source->addr + 12, 4);
        memmove(destination->addr, destination->addr + 12, 4);
        source->family = 4;
        destination->family = 4;
    }
    if (source->family != destination->family || (source->family != 4 && source->family != 6))
        return -1;
    return 0;
}

int ct_proxy_protocol_build_v1(uint8_t *out, size_t cap, size_t *length,
                               const ct_stream_metadata *metadata) {
#ifndef CONFIG_FEATURE_PROXY_PROTOCOL_V1
    (void)out;
    (void)cap;
    (void)length;
    (void)metadata;
    return -1;
#else
    ct_endpoint source, destination;
    char source_address[INET6_ADDRSTRLEN], destination_address[INET6_ADDRSTRLEN];
    if (normalize_pair(&source, &destination, metadata))
        return -1;
    int af = source.family == 4 ? AF_INET : AF_INET6;
    size_t address_length = source.family == 4 ? 4U : 16U;
    if (!inet_ntop(af, source.addr, source_address, sizeof source_address) ||
        !inet_ntop(af, destination.addr, destination_address, sizeof destination_address))
        return -1;
    int n = snprintf((char *)out, cap, "PROXY TCP%u %s %s %u %u\r\n", source.family, source_address,
                     destination_address, source.port, destination.port);
    if (n < 0 || (size_t)n >= cap || (size_t)n > CT_PROXY_PROTOCOL_V1_MAX_HEADER)
        return -1;
    (void)address_length;
    *length = (size_t)n;
    return 0;
#endif
}

int ct_proxy_protocol_build_v2(uint8_t *out, size_t cap, size_t *length,
                               const ct_stream_metadata *metadata) {
#ifndef CONFIG_FEATURE_PROXY_PROTOCOL_V2
    (void)out;
    (void)cap;
    (void)length;
    (void)metadata;
    return -1;
#else
    static const uint8_t signature[CT_PROXY_PROTOCOL_V2_SIGNATURE_SIZE] = {
        0x0d, 0x0a, 0x0d, 0x0a, 0x00, 0x0d, 0x0a, 0x51, 0x55, 0x49, 0x54, 0x0a};
    ct_endpoint source, destination;
    if (normalize_pair(&source, &destination, metadata))
        return -1;
    size_t address_length = source.family == 4 ? 4U : 16U;
    uint16_t payload_length = (uint16_t)(address_length * 2U + 4U);
    size_t total = CT_PROXY_PROTOCOL_V2_SIGNATURE_SIZE + 4U + payload_length;
    if (cap < total)
        return -1;
    memcpy(out, signature, sizeof signature);
    out[12] = 0x21;                             /* version 2, PROXY command */
    out[13] = source.family == 4 ? 0x11 : 0x21; /* TCP over IPv4/IPv6 */
    ct_put_u16(out + 14, payload_length);
    memcpy(out + 16, source.addr, address_length);
    memcpy(out + 16 + address_length, destination.addr, address_length);
    ct_put_u16(out + 16 + address_length * 2U, source.port);
    ct_put_u16(out + 16 + address_length * 2U + 2U, destination.port);
    *length = total;
    return 0;
#endif
}

int ct_proxy_protocol_build(uint8_t *out, size_t cap, size_t *length,
                            const ct_stream_metadata *metadata) {
    if (!metadata || metadata->proxy_protocol == CT_PROXY_PROTOCOL_OFF) {
        *length = 0;
        return 0;
    }
    if (metadata->proxy_protocol == CT_PROXY_PROTOCOL_V1)
        return ct_proxy_protocol_build_v1(out, cap, length, metadata);
    if (metadata->proxy_protocol == CT_PROXY_PROTOCOL_V2)
        return ct_proxy_protocol_build_v2(out, cap, length, metadata);
    return -1;
}
