#ifndef CT_APP_RUNTIME_H
#define CT_APP_RUNTIME_H

#include "generated/autoconf.h"

void ct_runtime_init(void);
int ct_runtime_should_stop(void);
#ifdef CONFIG_FEATURE_TEST_HOOKS
void ct_test_reset_runtime_stop(void);
#endif

#endif
