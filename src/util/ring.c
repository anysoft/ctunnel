#include "util/ring.h"
#include "ctunnel.h"
#include <stdlib.h>
#include <string.h>
int ct_ring_init(ct_ring *r, size_t cap) {
    CT_ASSERT(r != NULL && cap != 0);
    r->data = (uint8_t *)malloc(cap);
    if (!r->data)
        return -1;
    r->cap = cap;
    r->head = r->len = 0;
    return 0;
}
void ct_ring_free(ct_ring *r) {
    if (r->data) {
        memset(r->data, 0, r->cap);
        free(r->data);
    }
    memset(r, 0, sizeof *r);
}
size_t ct_ring_write(ct_ring *r, const void *src, size_t n) {
    CT_ASSERT(r != NULL && r->data != NULL && r->head < r->cap && r->len <= r->cap);
    if (n > r->cap - r->len)
        n = r->cap - r->len;
    size_t tail = (r->head + r->len) % r->cap, a = r->cap - tail;
    if (a > n)
        a = n;
    memcpy(r->data + tail, src, a);
    memcpy(r->data, (const uint8_t *)src + a, n - a);
    r->len += n;
    return n;
}
size_t ct_ring_read(ct_ring *r, void *dst, size_t n) {
    CT_ASSERT(r != NULL && r->data != NULL && r->head < r->cap && r->len <= r->cap);
    if (n > r->len)
        n = r->len;
    size_t a = r->cap - r->head;
    if (a > n)
        a = n;
    memcpy(dst, r->data + r->head, a);
    memcpy((uint8_t *)dst + a, r->data, n - a);
    r->head = (r->head + n) % r->cap;
    r->len -= n;
    return n;
}
size_t ct_ring_peek(const ct_ring *r, const uint8_t **p) {
    *p = r->data + r->head;
    size_t n = r->cap - r->head;
    return n < r->len ? n : r->len;
}
void ct_ring_consume(ct_ring *r, size_t n) {
    if (n > r->len)
        n = r->len;
    r->head = (r->head + n) % r->cap;
    r->len -= n;
}
