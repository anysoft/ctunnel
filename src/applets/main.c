#include "applets/applets.h"
#include "ctunnel/crypto.h"
#include "platform/platform.h"
#include <stdio.h>
#include <string.h>

typedef int (*applet_fn)(int argc, char **argv);

typedef struct {
    const char *name;
    applet_fn function;
} applet_entry;

static const char *base_name(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    return base;
}

static const applet_entry applets[] = {
#ifdef CONFIG_CTUNNEL_SERVER
    {"server", ct_applet_server},
#endif
#ifdef CONFIG_CTUNNEL_CLIENT
    {"client", ct_applet_client},
#endif
#ifdef CONFIG_FEATURE_KEYGEN
    {"keygen", ct_applet_keygen},
#endif
#ifdef CONFIG_FEATURE_FINGERPRINT
    {"fingerprint", ct_applet_fingerprint},
#endif
#ifdef CONFIG_FEATURE_CONFIG_TEST
    {"configtest", ct_applet_configtest},
#endif
#ifdef CONFIG_FEATURE_BUILD_INFO
    {"build-info", ct_applet_build_info},   {"build-config", ct_applet_build_config},
#endif
};

static applet_fn find_applet(const char *name) {
    for (size_t i = 0; i < sizeof applets / sizeof applets[0]; i++)
        if (!strcmp(name, applets[i].name))
            return applets[i].function;
    return NULL;
}

void ct_applet_usage(FILE *stream) {
    fputs("ctunnel - lightweight authenticated reverse TCP tunnel\n\nusage:\n", stream);
#ifdef CONFIG_CTUNNEL_SERVER
    fputs("  ctunnel server -c FILE\n", stream);
#endif
#ifdef CONFIG_CTUNNEL_CLIENT
    fputs("  ctunnel client -c FILE\n", stream);
#endif
#ifdef CONFIG_FEATURE_CONFIG_TEST
    fputs("  ctunnel configtest -c FILE\n", stream);
#endif
#ifdef CONFIG_FEATURE_KEYGEN
    fputs("  ctunnel keygen --private FILE --public FILE\n", stream);
#endif
#ifdef CONFIG_FEATURE_FINGERPRINT
    fputs("  ctunnel fingerprint PUBLIC_FILE\n", stream);
#endif
#ifdef CONFIG_FEATURE_BUILD_INFO
    fputs("  ctunnel build-info | build-config\n", stream);
#endif
    fputs("  ctunnel --version | --help\n", stream);
#ifdef CONFIG_FEATURE_VERBOSE_HELP
    fputs("\nRole applets may also be selected with ctunnel-server and ctunnel-client argv[0] "
          "aliases.\n",
          stream);
#endif
}

int main(int argc, char **argv) {
    if (ct_platform_init() || ct_crypto_init()) {
        fputs("platform/crypto initialization failed\n", stderr);
        return 1;
    }
    int result = 0;
    const char *program = base_name(argv[0]);
    applet_fn function = NULL;
#ifdef CONFIG_CTUNNEL_SERVER
    if (!strcmp(program, "ctunnel-server"))
        function = ct_applet_server;
#endif
#ifdef CONFIG_CTUNNEL_CLIENT
    if (!strcmp(program, "ctunnel-client"))
        function = ct_applet_client;
#endif
    if (function)
        result = function(argc, argv);
    else if (argc > 1 && (function = find_applet(argv[1])) != NULL)
        result = function(argc - 1, argv + 1);
    else if (argc == 2 && (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version")))
        ct_print_version();
    else if (argc == 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")))
        ct_applet_usage(stdout);
#ifdef CONFIG_FEATURE_BUILD_INFO
    else if (argc == 2 && !strcmp(argv[1], "--build-info"))
        result = ct_applet_build_info(argc, argv);
#endif
    else if (argc > 1 && argv[1][0] == '-')
        result = ct_applet_legacy(argc, argv);
    else {
        if (argc > 1)
            fprintf(stderr, "unknown or unavailable applet: %s\n", argv[1]);
        ct_applet_usage(stderr);
        result = 2;
    }
    ct_platform_cleanup();
    return result;
}
