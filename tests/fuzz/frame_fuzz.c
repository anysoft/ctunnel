#include "protocol/protocol.h"
#include <stddef.h>
#include <stdint.h>
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    if (n >= CT_FRAME_HEADER_SIZE) {
        ct_frame_header h;
        (void)ct_frame_header_decode(d, &h);
    }
    return 0;
}
