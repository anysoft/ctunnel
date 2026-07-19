#include "ctunnel/version.h"
#include "applets/applets.h"
#include "ctunnel/crypto.h"
#include <stdio.h>

void ct_print_version(void) {
    printf("ctunnel %s\ngit commit: %s\ntarget: %s\ncrypto backend: %s %s\n", CTUNNEL_VERSION,
           CTUNNEL_GIT_COMMIT, CTUNNEL_TARGET, ct_crypto_backend_name(),
           ct_crypto_backend_version());
}
