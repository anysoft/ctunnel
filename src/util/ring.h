#ifndef CT_RING_H
#define CT_RING_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
    uint8_t *data;
    size_t cap, head, len;
} ct_ring;
int ct_ring_init(ct_ring *, size_t);
void ct_ring_free(ct_ring *);
size_t ct_ring_write(ct_ring *, const void *, size_t);
size_t ct_ring_read(ct_ring *, void *, size_t);
size_t ct_ring_peek(const ct_ring *, const uint8_t **);
void ct_ring_consume(ct_ring *, size_t);
#endif
