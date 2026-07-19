#include "applets/applets.h"
#include <stdio.h>

int ct_applet_configtest(int argc, char **argv) {
    ct_run_options options;
    int result = ct_applet_parse_run_options(argc, argv, &options);
    if (result)
        return result;
    ct_config config;
    char error[512];
    if (ct_applet_load_config(&options, CT_MODE_NONE, &config, error, sizeof error)) {
        fprintf(stderr, "configtest: configuration error: %s\n", error);
        return 1;
    }
#ifdef CONFIG_FEATURE_DATA_ENCRYPTION
    for (size_t i = 0; i < config.service_count; i++)
        if (config.services[i].encryption == CT_ENC_DISABLED)
            fprintf(stderr, "warning: service '%s' sends relay data without encryption\n",
                    config.services[i].id);
#endif
    printf("configuration OK (%s, %zu service(s), %zu authorized client(s))\n",
           config.mode == CT_MODE_SERVER ? "server" : "client", config.service_count,
           config.client_count);
    return 0;
}
