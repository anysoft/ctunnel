#include "platform/event.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct {
    ct_socket fd;
    int events;
    void *user;
} registration;

struct ct_event_loop {
    int queue;
    registration *registrations;
    size_t length;
    size_t capacity;
};

static int find_registration(const ct_event_loop *loop, ct_socket fd) {
    for (size_t i = 0; i < loop->length; i++)
        if (loop->registrations[i].fd == fd)
            return (int)i;
    return -1;
}

static int change_filter(ct_event_loop *loop, ct_socket fd, int filter, int flags, void *user) {
    struct kevent change;
    EV_SET(&change, (uintptr_t)fd, filter, flags, 0, 0, user);
    if (kevent(loop->queue, &change, 1, NULL, 0, NULL) == 0)
        return 0;
    return (flags & EV_DELETE) && errno == ENOENT ? 0 : -1;
}

static int apply_events(ct_event_loop *loop, ct_socket fd, int old_events, int new_events,
                        void *user) {
    if ((old_events & CT_EV_READ) != (new_events & CT_EV_READ) &&
        change_filter(loop, fd, EVFILT_READ,
                      (new_events & CT_EV_READ) ? EV_ADD | EV_ENABLE : EV_DELETE, user))
        return -1;
    if ((old_events & CT_EV_WRITE) != (new_events & CT_EV_WRITE) &&
        change_filter(loop, fd, EVFILT_WRITE,
                      (new_events & CT_EV_WRITE) ? EV_ADD | EV_ENABLE : EV_DELETE, user))
        return -1;
    return 0;
}

ct_event_loop *event_loop_create(size_t capacity) {
    ct_event_loop *loop = calloc(1, sizeof *loop);
    if (!loop)
        return NULL;
    loop->queue = kqueue();
    loop->registrations = calloc(capacity, sizeof *loop->registrations);
    loop->capacity = capacity;
    if (loop->queue < 0 || !loop->registrations) {
        event_loop_destroy(loop);
        return NULL;
    }
    return loop;
}

int event_loop_add(ct_event_loop *loop, ct_socket fd, int events, void *user) {
    if (loop->length == loop->capacity || find_registration(loop, fd) >= 0 ||
        apply_events(loop, fd, 0, events, user))
        return -1;
    loop->registrations[loop->length++] = (registration){fd, events, user};
    return 0;
}

int event_loop_modify(ct_event_loop *loop, ct_socket fd, int events, void *user) {
    int index = find_registration(loop, fd);
    if (index < 0 || apply_events(loop, fd, loop->registrations[index].events, events, user))
        return -1;
    loop->registrations[index].events = events;
    loop->registrations[index].user = user;
    return 0;
}

int event_loop_delete(ct_event_loop *loop, ct_socket fd) {
    int index = find_registration(loop, fd);
    if (index < 0)
        return -1;
    (void)apply_events(loop, fd, loop->registrations[index].events, 0, NULL);
    loop->registrations[index] = loop->registrations[--loop->length];
    return 0;
}

int event_loop_wait(ct_event_loop *loop, ct_event *events, size_t capacity, int timeout_ms) {
    struct kevent *ready = calloc(capacity, sizeof *ready);
    if (!ready)
        return -1;
    struct timespec timeout = {timeout_ms / 1000, (timeout_ms % 1000) * 1000000L};
    int count = kevent(loop->queue, NULL, 0, ready, (int)capacity, &timeout);
    if (count > 0) {
        for (int i = 0; i < count; i++) {
            events[i].fd = (ct_socket)ready[i].ident;
            events[i].events = ready[i].filter == EVFILT_WRITE ? CT_EV_WRITE : CT_EV_READ;
            if (ready[i].flags & EV_EOF)
                events[i].events |= CT_EV_READ | CT_EV_HANGUP;
            if (ready[i].flags & EV_ERROR)
                events[i].events |= CT_EV_READ | CT_EV_ERROR;
            events[i].user = ready[i].udata;
        }
    }
    free(ready);
    return count;
}

void event_loop_destroy(ct_event_loop *loop) {
    if (!loop)
        return;
    if (loop->queue >= 0)
        close(loop->queue);
    free(loop->registrations);
    free(loop);
}
