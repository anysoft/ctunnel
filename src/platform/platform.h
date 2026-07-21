#ifndef CT_PLATFORM_H
#define CT_PLATFORM_H
#include "generated/autoconf.h"
#include <stddef.h>
#include <stdint.h>
#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET ct_socket;
#define CT_INVALID_SOCKET INVALID_SOCKET
#else
typedef int ct_socket;
#define CT_INVALID_SOCKET (-1)
#endif
int ct_platform_init(void);
void ct_platform_cleanup(void);
uint64_t ct_monotonic_ms(void);
int ct_platform_random(uint8_t *, size_t);
int ct_socket_nonblock(ct_socket);
int ct_socket_keepalive(ct_socket);
void ct_socket_close(ct_socket);
int ct_socket_would_block(void);
void ct_secure_zero(void *, size_t);
void ct_sleep_ms(unsigned);
void ct_fd_limit_diagnostics(unsigned recommended, unsigned long long *soft,
                             unsigned long long *hard);
#endif
