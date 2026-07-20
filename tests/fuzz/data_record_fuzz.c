#include "protocol/protocol.h"
#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size >= 12) {
        uint32_t encoded_length;
        uint64_t sequence;
        uint64_t previous = size >= 20 ? ct_get_u64(data + 12) : 0;
        (void)ct_data_record_header_decode(data, 16384, previous, &encoded_length, &sequence);
    }
    return 0;
}
