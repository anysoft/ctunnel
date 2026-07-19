#include "applets/applets.h"

int ct_applet_server(int argc, char **argv) {
    return ct_applet_run_role(argc, argv, CT_MODE_SERVER, ct_run_server);
}
