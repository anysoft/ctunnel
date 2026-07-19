#include "platform/event.h"
#if !defined(__linux__)
#error "epoll backend requires Linux"
#endif
#include <errno.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>
typedef struct {
    ct_socket fd;
    void *user;
    uint32_t flags;
} item;
struct ct_event_loop {
    int ep;
    item *items;
    size_t len, cap;
};
ct_event_loop *event_loop_create(size_t cap) {
    ct_event_loop *l = calloc(1, sizeof *l);
    if (!l)
        return NULL;
    l->ep = epoll_create1(EPOLL_CLOEXEC);
    l->items = calloc(cap, sizeof *l->items);
    l->cap = cap;
    if (l->ep < 0 || !l->items) {
        event_loop_destroy(l);
        return NULL;
    }
    return l;
}
static int find(ct_event_loop *l, ct_socket f) {
    for (size_t i = 0; i < l->len; i++)
        if (l->items[i].fd == f)
            return (int)i;
    return -1;
}
static uint32_t flags(int e) {
    return (uint32_t)(((e & CT_EV_READ) ? EPOLLIN : 0) | ((e & CT_EV_WRITE) ? EPOLLOUT : 0) |
                      EPOLLRDHUP);
}
int event_loop_add(ct_event_loop *l, ct_socket f, int ev, void *u) {
    if (l->len == l->cap || find(l, f) >= 0)
        return -1;
    item *i = &l->items[l->len++];
    i->fd = f;
    i->user = u;
    i->flags = flags(ev);
    struct epoll_event ee = {i->flags, {.ptr = i}};
    if (epoll_ctl(l->ep, EPOLL_CTL_ADD, f, &ee)) {
        l->len--;
        return -1;
    }
    return 0;
}
int event_loop_modify(ct_event_loop *l, ct_socket f, int ev, void *u) {
    int n = find(l, f);
    if (n < 0)
        return -1;
    l->items[n].user = u;
    l->items[n].flags = flags(ev);
    struct epoll_event ee = {l->items[n].flags, {.ptr = &l->items[n]}};
    return epoll_ctl(l->ep, EPOLL_CTL_MOD, f, &ee);
}
int event_loop_delete(ct_event_loop *l, ct_socket f) {
    int n = find(l, f);
    if (n < 0)
        return -1;
    (void)epoll_ctl(l->ep, EPOLL_CTL_DEL, f, NULL);
    l->items[n] = l->items[--l->len];
    if ((size_t)n < l->len) {
        struct epoll_event ee = {l->items[n].flags, {.ptr = &l->items[n]}};
        (void)epoll_ctl(l->ep, EPOLL_CTL_MOD, l->items[n].fd, &ee);
    }
    return 0;
}
int event_loop_wait(ct_event_loop *l, ct_event *out, size_t cap, int ms) {
    struct epoll_event *e = calloc(cap, sizeof *e);
    if (!e)
        return -1;
    int n;
    do {
        n = epoll_wait(l->ep, e, (int)cap, ms);
    } while (n < 0 && errno == EINTR);
    for (int x = 0; x < n; x++) {
        item *i = e[x].data.ptr;
        out[x].fd = i->fd;
        out[x].user = i->user;
        out[x].events =
            ((e[x].events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) ? CT_EV_READ : 0) |
            ((e[x].events & EPOLLOUT) ? CT_EV_WRITE : 0);
    }
    free(e);
    return n;
}
void event_loop_destroy(ct_event_loop *l) {
    if (l) {
        if (l->ep >= 0)
            close(l->ep);
        free(l->items);
        free(l);
    }
}
