#include "platform/platform.h"
#include "ctunnel/crypto.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <bcrypt.h>
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#endif
int ct_platform_init(void) {
#ifdef _WIN32
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0)
        return -1;
#endif
    return 0;
}
void ct_platform_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}
uint64_t ct_monotonic_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
#endif
}
int ct_platform_random(uint8_t *p, size_t n) {
#ifdef _WIN32
    if (n > ULONG_MAX)
        return -1;
    return BCryptGenRandom(NULL, (PUCHAR)p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0
                                                                                            : -1;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    arc4random_buf(p, n);
    return 0;
#else
    size_t offset = 0;
#if defined(__linux__)
    while (offset < n) {
        ssize_t got = getrandom(p + offset, n - offset, 0);
        if (got > 0) {
            offset += (size_t)got;
            continue;
        }
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0 && (errno == ENOSYS || errno == EAGAIN))
            break;
        return -1;
    }
    if (offset == n)
        return 0;
#endif
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open("/dev/urandom", flags);
    if (fd < 0)
        return -1;
    while (offset < n) {
        ssize_t got = read(fd, p + offset, n - offset);
        if (got > 0) {
            offset += (size_t)got;
            continue;
        }
        if (got < 0 && errno == EINTR)
            continue;
        close(fd);
        return -1;
    }
    if (close(fd) != 0)
        return -1;
    return 0;
#endif
}
int ct_socket_nonblock(ct_socket s) {
#ifdef _WIN32
    u_long one = 1;
    return ioctlsocket(s, FIONBIO, &one) == 0 ? 0 : -1;
#else
    int f = fcntl(s, F_GETFL, 0);
    return f < 0 || fcntl(s, F_SETFL, f | O_NONBLOCK) < 0 ? -1 : 0;
#endif
}
int ct_socket_keepalive(ct_socket s) {
    int one = 1;
    if (setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char *)&one, sizeof one) != 0)
        return -1;
#if defined(TCP_KEEPIDLE)
    int idle = 60, int intvl = 20, cnt = 3;
    (void)setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    (void)setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    (void)setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
#endif
    return 0;
}
void ct_socket_close(ct_socket s) {
    if (s == CT_INVALID_SOCKET)
        return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}
int ct_socket_would_block(void) {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
#endif
}
void ct_secure_zero(void *p, size_t n) {
    ct_crypto_wipe(p, n);
}
void ct_sleep_ms(unsigned n) {
#ifdef _WIN32
    Sleep(n);
#else
    struct timespec t = {(time_t)(n / 1000u), (long)(n % 1000u) * 1000000L};
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {
    }
#endif
}
