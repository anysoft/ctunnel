#ifndef CTUNNEL_H
#define CTUNNEL_H
#include "generated/autoconf.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef CONFIG_FEATURE_ASSERTIONS
#include <assert.h>
#define CT_ASSERT(expression) assert(expression)
#else
#define CT_ASSERT(expression) ((void)sizeof(expression))
#endif

#define CT_MAX_CLIENT_ID CONFIG_MAX_CLIENT_ID_LENGTH
#define CT_MAX_SERVICE_ID CONFIG_MAX_SERVICE_ID_LENGTH
#define CT_MAX_ADDR CONFIG_MAX_ADDRESS_LENGTH
#define CT_MAX_PATH CONFIG_MAX_PATH_LENGTH
#define CT_MAX_SERVICES CONFIG_MAX_SERVICES
#define CT_MAX_AUTH_CLIENTS CONFIG_MAX_CLIENTS
#define CT_MAX_PORT_RANGES CONFIG_MAX_PORT_RANGES
#define CT_MAX_FRAME_PAYLOAD CONFIG_MAX_FRAME_SIZE
#define CT_MAX_UDP_SESSIONS CONFIG_MAX_UDP_SESSIONS
#define CT_MAX_UDP_DATAGRAM CONFIG_MAX_UDP_DATAGRAM_SIZE
#define CT_FRAME_HEADER_SIZE 36U
#define CT_IO_BUFFER_SIZE CONFIG_STREAM_BUFFER_SIZE
#define CT_CONTROL_BUFFER_SIZE CONFIG_CONTROL_BUFFER_SIZE

typedef enum { CT_MODE_NONE, CT_MODE_SERVER, CT_MODE_CLIENT } ct_mode;
typedef enum { CT_ENC_REQUIRED, CT_ENC_DISABLED } ct_enc_mode;
typedef enum { CT_CIPHER_NONE = 0, CT_CIPHER_CHACHA = 1 } ct_cipher;
typedef enum {
    CT_PROXY_PROTOCOL_OFF = 0,
    CT_PROXY_PROTOCOL_V1 = 1,
    CT_PROXY_PROTOCOL_V2 = 2
} ct_proxy_protocol_mode;

typedef struct {
    uint16_t first, last;
} ct_port_range;
typedef struct {
    uint8_t family;
    uint8_t addr[16];
    uint16_t port;
} ct_endpoint;
typedef struct {
    ct_proxy_protocol_mode proxy_protocol;
    ct_endpoint source, destination;
} ct_stream_metadata;
typedef struct {
    char id[CT_MAX_SERVICE_ID + 1], remote_addr[CT_MAX_ADDR + 1], local_addr[CT_MAX_ADDR + 1];
    uint16_t remote_port, local_port;
    ct_enc_mode encryption;
    int type, proxy_protocol;
    int udp_idle_timeout, udp_reply_timeout, udp_max_sessions, udp_max_datagram_size,
        udp_options_seen;
} ct_service_config;
typedef struct {
    char id[CT_MAX_CLIENT_ID + 1], public_key[CT_MAX_PATH], allow_addr[CT_MAX_ADDR + 1];
    ct_port_range ports[CT_MAX_PORT_RANGES];
    size_t port_count;
    int max_services, max_streams;
} ct_authorized_client;
typedef struct {
    ct_mode mode;
    char config_path[CT_MAX_PATH];
    char bind_addr[CT_MAX_ADDR + 1], server_addr[CT_MAX_ADDR + 1];
    uint16_t bind_port, server_port;
    char client_id[CT_MAX_CLIENT_ID + 1], identity_private_key[CT_MAX_PATH],
        server_public_key[CT_MAX_PATH], authorized_clients_file[CT_MAX_PATH], log_file[CT_MAX_PATH];
    unsigned cipher_mask;
    ct_cipher preferred_cipher;
    int heartbeat_interval, heartbeat_timeout, handshake_timeout, connect_timeout;
    int reconnect_initial_delay, reconnect_max_delay, reconnect_jitter_percent, pool_count;
    int max_clients, max_services_per_client, max_streams_per_client, max_pending_streams;
    ct_enc_mode default_data_encryption;
    int log_level, log_rotate_days, log_max_size_kib;
    ct_service_config services[CT_MAX_SERVICES];
    size_t service_count;
    ct_authorized_client clients[CT_MAX_AUTH_CLIENTS];
    size_t client_count;
} ct_config;

int ct_config_load(const char *path, ct_config *out, char *err, size_t errlen);
int ct_authorized_load(const char *path, ct_config *cfg, char *err, size_t errlen);
int ct_config_validate(const ct_config *cfg, char *err, size_t errlen);
int ct_config_validate_security_files(const ct_config *cfg, char *err, size_t errlen);
const ct_authorized_client *ct_authorized_find(const ct_config *, const char *id);
bool ct_authorized_port(const ct_authorized_client *, const char *addr, uint16_t port);

#ifdef CONFIG_CTUNNEL_SERVER
int ct_run_server(const ct_config *cfg);
#endif
#ifdef CONFIG_CTUNNEL_CLIENT
int ct_run_client(const ct_config *cfg);
#endif
#endif
