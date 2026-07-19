#include "applets/applets.h"

int ct_applet_client(int argc, char **argv) {
    return ct_applet_run_role(argc, argv, CT_MODE_CLIENT, ct_run_client);
}
