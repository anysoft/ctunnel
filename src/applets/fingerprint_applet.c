#include "applets/applets.h"
#include "ctunnel/crypto.h"
#include <stdio.h>

int ct_applet_fingerprint(int argc, char **argv) {
    if (argc != 2) {
        fputs("usage: ctunnel fingerprint PUBLIC_FILE\n", stderr);
        return 2;
    }
    char fingerprint[96];
    if (ct_fingerprint_file(argv[1], fingerprint, sizeof fingerprint)) {
        fputs("invalid public key\n", stderr);
        return 1;
    }
    puts(fingerprint);
    return 0;
}
