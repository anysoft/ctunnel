#include "applets/applets.h"
#include <stdio.h>

int ct_applet_legacy(int argc, char **argv) {
    ct_run_options options;
    int result = ct_applet_parse_run_options(argc, argv, &options);
    if (result)
        return result;
#ifdef CONFIG_FEATURE_CONFIG_TEST
    if (options.test_only)
        return ct_applet_configtest(argc, argv);
#endif
    ct_config config;
    char error[512];
    if (ct_applet_load_config(&options, CT_MODE_NONE, &config, error, sizeof error)) {
        fprintf(stderr, "ctunnel: configuration error: %s\n", error);
        return 1;
    }
#ifdef CONFIG_CTUNNEL_SERVER
    if (config.mode == CT_MODE_SERVER)
        return ct_applet_server(argc, argv);
#endif
#ifdef CONFIG_CTUNNEL_CLIENT
    if (config.mode == CT_MODE_CLIENT)
        return ct_applet_client(argc, argv);
#endif
    fputs("ctunnel: configured role is not compiled into this binary\n", stderr);
    return 1;
}
