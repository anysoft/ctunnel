#define _POSIX_C_SOURCE 200809L
#include "ctunnel/crypto.h"
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 2048)
        return 0;
    char path[] = "/tmp/ctunnel-key-fuzz-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return 0;
    size_t offset = 0;
    while (offset < size) {
        ssize_t written = write(fd, data + offset, size - offset);
        if (written <= 0)
            break;
        offset += (size_t)written;
    }
    (void)fchmod(fd, 0600);
    close(fd);
    uint8_t public_key[CT_ED_PUBLIC], private_key[CT_ED_SECRET];
    (void)ct_load_public_key(path, public_key);
    (void)ct_load_private_key(path, private_key);
    ct_crypto_wipe(public_key, sizeof public_key);
    ct_crypto_wipe(private_key, sizeof private_key);
    unlink(path);
    return 0;
}
