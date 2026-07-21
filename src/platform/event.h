#ifndef CT_EVENT_H
#define CT_EVENT_H
#include "generated/autoconf.h"
#include "platform/platform.h"
#include <stddef.h>
#define CT_EV_READ 1
#define CT_EV_WRITE 2
#define CT_EV_ERROR 4
#define CT_EV_HANGUP 8
#define CT_EV_TIMER 16
typedef struct ct_event_loop ct_event_loop;
typedef struct {
    ct_socket fd;
    int events;
    void *user;
} ct_event;
ct_event_loop *event_loop_create(size_t cap);
int event_loop_add(ct_event_loop *, ct_socket, int, void *);
int event_loop_modify(ct_event_loop *, ct_socket, int, void *);
int event_loop_delete(ct_event_loop *, ct_socket);
int event_loop_wait(ct_event_loop *, ct_event *, size_t, int);
void event_loop_destroy(ct_event_loop *);
#endif
