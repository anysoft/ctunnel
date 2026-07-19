#include "platform/event.h"
#include <stdlib.h>
#ifdef _WIN32
#define poll WSAPoll
typedef WSAPOLLFD ct_pollfd;
typedef ULONG ct_nfds;
#else
#include <poll.h>
typedef struct pollfd ct_pollfd;
typedef nfds_t ct_nfds;
#endif
struct ct_event_loop {
    ct_pollfd *fds;
    void **users;
    size_t len, cap;
};
ct_event_loop *event_loop_create(size_t c) {
    ct_event_loop *l = calloc(1, sizeof *l);
    if (!l)
        return NULL;
    l->fds = calloc(c, sizeof *l->fds);
    l->users = calloc(c, sizeof *l->users);
    if (!l->fds || !l->users) {
        event_loop_destroy(l);
        return NULL;
    }
    l->cap = c;
    return l;
}
static int idx(ct_event_loop *l, ct_socket fd) {
    for (size_t i = 0; i < l->len; i++)
        if (l->fds[i].fd == fd)
            return (int)i;
    return -1;
}
int event_loop_add(ct_event_loop *l, ct_socket fd, int ev, void *u) {
    if (l->len == l->cap || idx(l, fd) >= 0)
        return -1;
    size_t i = l->len++;
    l->fds[i].fd = fd;
    l->fds[i].events =
        (short)(((ev & CT_EV_READ) ? POLLIN : 0) | ((ev & CT_EV_WRITE) ? POLLOUT : 0));
    l->users[i] = u;
    return 0;
}
int event_loop_modify(ct_event_loop *l, ct_socket fd, int ev, void *u) {
    int i = idx(l, fd);
    if (i < 0)
        return -1;
    l->fds[i].events = (short)(((ev & 1) ? POLLIN : 0) | ((ev & 2) ? POLLOUT : 0));
    l->users[i] = u;
    return 0;
}
int event_loop_delete(ct_event_loop *l, ct_socket fd) {
    int i = idx(l, fd);
    if (i < 0)
        return -1;
    size_t z = --l->len;
    l->fds[i] = l->fds[z];
    l->users[i] = l->users[z];
    return 0;
}
int event_loop_wait(ct_event_loop *l, ct_event *out, size_t cap, int ms) {
    int n = poll(l->fds, (ct_nfds)l->len, ms);
    if (n <= 0)
        return n;
    size_t k = 0;
    for (size_t i = 0; i < l->len && k < cap; i++)
        if (l->fds[i].revents) {
            out[k].fd = l->fds[i].fd;
            out[k].events = ((l->fds[i].revents & (POLLIN | POLLHUP | POLLERR)) ? 1 : 0) |
                            ((l->fds[i].revents & POLLOUT) ? 2 : 0);
            out[k++].user = l->users[i];
            l->fds[i].revents = 0;
        }
    return (int)k;
}
void event_loop_destroy(ct_event_loop *l) {
    if (l) {
        free(l->fds);
        free(l->users);
        free(l);
    }
}
