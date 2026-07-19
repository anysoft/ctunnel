#include "applets/applets.h"
#include "ctunnel/crypto.h"
#include <stdio.h>
#include <string.h>

int ct_applet_keygen(int argc, char **argv) {
    const char *private_path = NULL;
    const char *public_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--private") && i + 1 < argc)
            private_path = argv[++i];
        else if (!strcmp(argv[i], "--public") && i + 1 < argc)
            public_path = argv[++i];
        else {
            fprintf(stderr, "keygen: unknown or incomplete option %s\n", argv[i]);
            return 2;
        }
    }
    if (!private_path || !public_path) {
        fputs("keygen requires --private and --public\n", stderr);
        return 2;
    }
    char fingerprint[96];
    if (ct_keygen_files(private_path, public_path, fingerprint, sizeof fingerprint)) {
        fputs("key generation failed; output files must not already exist\n", stderr);
        return 1;
    }
    printf("public key fingerprint: %s\n", fingerprint);
    return 0;
}
