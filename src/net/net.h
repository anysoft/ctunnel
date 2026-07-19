#ifndef CT_NET_H
#define CT_NET_H
#include "ctunnel.h"
#include "ctunnel/crypto.h"
#include "platform/platform.h"
#include "protocol/protocol.h"
typedef struct {
    ct_socket fd;
    ct_cipher cipher;
    ct_session_keys keys;
    uint64_t tx_seq, rx_seq;
    uint64_t work_tx_seq, work_rx_high, work_rx_bitmap;
    int is_client;
    char client_id[CT_MAX_CLIENT_ID + 1];
    uint64_t last_rx_ms, last_tx_ms;
} ct_control;
ct_socket ct_net_listen(const char *, uint16_t, int);
ct_socket ct_net_connect(const char *, uint16_t, int);
ct_socket ct_net_accept(ct_socket, char *, size_t);
int ct_plain_send(ct_socket, uint8_t, uint64_t, uint64_t, const uint8_t *, size_t, int);
int ct_plain_recv(ct_socket, ct_frame_header *, uint8_t *, size_t, size_t *, int);
int ct_control_send(ct_control *, uint8_t, uint64_t, const uint8_t *, size_t, int);
int ct_control_recv(ct_control *, ct_frame_header *, uint8_t *, size_t, size_t *, int);
int ct_handshake_client(ct_socket, const ct_config *, ct_control *);
int ct_handshake_server(ct_socket, const ct_config *, ct_control *, const ct_authorized_client **);
int ct_work_connect(const ct_config *, ct_control *, ct_socket *);
int ct_work_accept_bind(ct_socket, ct_control *);
int ct_start_stream_send(ct_socket, const ct_control *, const char *, uint64_t, ct_enc_mode,
                         uint8_t[32]);
int ct_start_stream_recv(ct_socket, const ct_control *, char *, size_t, uint64_t *, ct_enc_mode *,
                         uint8_t[32]);
int ct_stream_ready_send(ct_socket, const ct_control *, uint64_t, const uint8_t[32], int);
int ct_stream_ready_recv(ct_socket, const ct_control *, uint64_t, const uint8_t[32], int *);
#endif
