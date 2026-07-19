#ifndef CT_APPLETS_H
#define CT_APPLETS_H

#include "ctunnel.h"
#include <stddef.h>
#include <stdio.h>

typedef struct {
    const char *config_path;
    const char *log_level;
    int test_only;
} ct_run_options;

int ct_applet_parse_run_options(int argc, char **argv, ct_run_options *options);
int ct_applet_load_config(const ct_run_options *options, ct_mode expected, ct_config *config,
                          char *error, size_t error_size);
int ct_applet_run_role(int argc, char **argv, ct_mode expected,
                       int (*runner)(const ct_config *config));
void ct_applet_usage(FILE *stream);
void ct_print_version(void);

int ct_applet_legacy(int argc, char **argv);
#ifdef CONFIG_CTUNNEL_SERVER
int ct_applet_server(int argc, char **argv);
#endif
#ifdef CONFIG_CTUNNEL_CLIENT
int ct_applet_client(int argc, char **argv);
#endif
#ifdef CONFIG_FEATURE_KEYGEN
int ct_applet_keygen(int argc, char **argv);
#endif
#ifdef CONFIG_FEATURE_FINGERPRINT
int ct_applet_fingerprint(int argc, char **argv);
#endif
#ifdef CONFIG_FEATURE_CONFIG_TEST
int ct_applet_configtest(int argc, char **argv);
#endif
#ifdef CONFIG_FEATURE_BUILD_INFO
int ct_applet_build_info(int argc, char **argv);
int ct_applet_build_config(int argc, char **argv);
#endif

#endif
