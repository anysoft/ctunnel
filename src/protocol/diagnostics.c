#include "protocol/diagnostics.h"
#include "util/log.h"

void ct_protocol_diagnostic(const char *reason) {
    CT_LOG_DEBUG("protocol", "rejected input: %s", reason);
}
