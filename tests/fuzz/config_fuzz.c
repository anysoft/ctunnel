#include "ctunnel.h"
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 65536)
        return 0;
    char path[] = "/tmp/ctunnel-config-fuzz-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return 0;
    size_t off = 0;
    while (off < size) {
        ssize_t n = write(fd, data + off, size - off);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    close(fd);
    ct_config cfg;
    char error[256];
    (void)ct_config_load(path, &cfg, error, sizeof error);
    unlink(path);
    return 0;
}
