#ifndef CT_RELAY_H
#define CT_RELAY_H
#include "ctunnel/crypto.h"
#include "platform/platform.h"
#include "util/ring.h"
typedef struct {
    ct_socket direct, work;
    int closed, direct_eof, work_eof;
#ifdef CONFIG_FEATURE_DATA_ENCRYPTION
    int encrypted;
    ct_cipher cipher;
    uint8_t send_key[32], recv_key[32];
    uint8_t send_nonce[CT_NONCE_BASE], recv_nonce[CT_NONCE_BASE];
    uint64_t send_seq, recv_seq;
    ct_ring work_input;
#endif
    ct_ring to_direct, to_work;
} ct_relay;
int ct_relay_init(ct_relay *, ct_socket, ct_socket, int, ct_enc_mode, ct_cipher, const uint8_t[32],
                  uint64_t, const uint8_t[32], const uint8_t[32], const char *);
void ct_relay_close(ct_relay *);
int ct_relay_events(const ct_relay *, ct_socket);
int ct_relay_process(ct_relay *, ct_socket, int);
#endif
