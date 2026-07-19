#include "protocol/protocol.h"
#include <stddef.h>
#include <stdint.h>
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    size_t o = 0;
    char id[CT_MAX_CLIENT_ID + 1];
    if (ct_unpack_string(d, n, &o, id, sizeof id) == 0 && n - o >= 68) {
        (void)ct_get_u32(d + o + 64);
    }
    return 0;
}
