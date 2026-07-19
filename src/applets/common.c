#include "applets/applets.h"
#include "util/log.h"
#include <stdio.h>
#include <string.h>

int ct_applet_parse_run_options(int argc, char **argv, ct_run_options *options) {
    memset(options, 0, sizeof *options);
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-c") || !strcmp(argv[i], "--config")) && i + 1 < argc)
            options->config_path = argv[++i];
        else if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--foreground")) {
#ifdef CONFIG_FEATURE_CONFIG_TEST
        } else if (!strcmp(argv[i], "-t") || !strcmp(argv[i], "--test-config")) {
            options->test_only = 1;
#endif
#ifdef CONFIG_FEATURE_COMMAND_LINE_OVERRIDE
        } else if (!strcmp(argv[i], "--log-level") && i + 1 < argc) {
            options->log_level = argv[++i];
#endif
        } else {
            fprintf(stderr, "%s: unknown or incomplete option: %s\n", argv[0], argv[i]);
            return 2;
        }
    }
    if (!options->config_path) {
        fprintf(stderr, "%s: configuration file is required (-c FILE)\n", argv[0]);
        return 2;
    }
    return 0;
}

int ct_applet_load_config(const ct_run_options *options, ct_mode expected, ct_config *config,
                          char *error, size_t error_size) {
    if (ct_config_load(options->config_path, config, error, error_size))
        return -1;
    if (expected != CT_MODE_NONE && config->mode != expected) {
        snprintf(error, error_size, "configuration mode does not match this applet");
        return -1;
    }
    if (config->mode == CT_MODE_SERVER &&
        ct_authorized_load(config->authorized_clients_file, config, error, error_size))
        return -1;
#ifdef CONFIG_FEATURE_COMMAND_LINE_OVERRIDE
    if (options->log_level) {
        int level = ct_log_parse_level(options->log_level);
        if (level < 0) {
            snprintf(error, error_size, "log level '%s' is unavailable in this build",
                     options->log_level);
            return -1;
        }
        config->log_level = level;
    }
#endif
    return 0;
}

int ct_applet_run_role(int argc, char **argv, ct_mode expected,
                       int (*runner)(const ct_config *config)) {
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
    if (ct_applet_load_config(&options, expected, &config, error, sizeof error)) {
        fprintf(stderr, "%s: configuration error: %s\n", argv[0], error);
        return 1;
    }
    ct_log_set_level(config.log_level);
    if (ct_log_configure(config.log_file, config.log_rotate_days)) {
        fprintf(stderr, "%s: cannot use log_file: %s\n", argv[0], config.log_file);
        return 1;
    }
    return runner(&config);
}
