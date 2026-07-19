#include "app/runtime.h"
#include "generated/autoconf.h"

#ifdef CONFIG_FEATURE_SIGNAL_HANDLING
#include <signal.h>
static volatile sig_atomic_t g_stopping;

static void stop_handler(int signal_number) {
    (void)signal_number;
    g_stopping = 1;
}
#else
static int g_stopping;
#endif

void ct_runtime_init(void) {
    g_stopping = 0;
#ifdef CONFIG_FEATURE_SIGNAL_HANDLING
    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
#endif
}

int ct_runtime_should_stop(void) {
    return g_stopping != 0;
}

#ifdef CONFIG_FEATURE_TEST_HOOKS
void ct_test_reset_runtime_stop(void) {
    g_stopping = 0;
}
#endif
