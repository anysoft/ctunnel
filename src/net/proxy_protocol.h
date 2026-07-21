#ifndef CT_PROXY_PROTOCOL_H
#define CT_PROXY_PROTOCOL_H
#include "ctunnel.h"
#include <stddef.h>
#include <stdint.h>

#define CT_PROXY_PROTOCOL_V1_MAX_HEADER 108U
#define CT_PROXY_PROTOCOL_V2_SIGNATURE_SIZE 12U
#define CT_PROXY_PROTOCOL_V2_IPV4_HEADER_SIZE 28U
#define CT_PROXY_PROTOCOL_V2_IPV6_HEADER_SIZE 52U
#define CT_PROXY_PROTOCOL_MAX_HEADER CT_PROXY_PROTOCOL_V1_MAX_HEADER

int ct_proxy_protocol_build_v1(uint8_t *, size_t, size_t *, const ct_stream_metadata *);
int ct_proxy_protocol_build_v2(uint8_t *, size_t, size_t *, const ct_stream_metadata *);
int ct_proxy_protocol_build(uint8_t *, size_t, size_t *, const ct_stream_metadata *);

#endif
