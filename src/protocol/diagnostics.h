#ifndef CT_PROTOCOL_DIAGNOSTICS_H
#define CT_PROTOCOL_DIAGNOSTICS_H

#include "generated/autoconf.h"

#ifdef CONFIG_FEATURE_PROTOCOL_DIAGNOSTICS
void ct_protocol_diagnostic(const char *reason);
#else
#define ct_protocol_diagnostic(...) ((void)0)
#endif

#endif
