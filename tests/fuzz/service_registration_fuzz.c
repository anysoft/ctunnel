#include "protocol/protocol.h"
#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char service[CT_MAX_SERVICE_ID + 1], address[CT_MAX_ADDR + 1];
    uint16_t port;
    uint8_t type, mode;
    (void)ct_register_request_decode(data, size, service, sizeof service, address, sizeof address,
                                     &port, &type, &mode, NULL, NULL, NULL, NULL, NULL);
    return 0;
}
