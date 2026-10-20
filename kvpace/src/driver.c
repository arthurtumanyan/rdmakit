/* driver.c - driver registry and connection settings, see driver.h. */
#include "driver.h"

#include <string.h>

static const char *g_auth;
static unsigned g_db;

const pace_driver *pace_driver_find(const char *name) {
    if (strcmp(name, "resp") == 0) {
        return &pace_driver_resp;
    }
    if (strcmp(name, "memcached") == 0) {
        return &pace_driver_memcached;
    }
#ifdef PACE_HAVE_NATIVE
    if (strcmp(name, "native") == 0) {
        return &pace_driver_native;
    }
#endif
    return NULL;
}

void pace_driver_set_auth(const char *secret) {
    g_auth = secret;
}

const char *pace_driver_auth(void) {
    return g_auth;
}

void pace_driver_set_db(unsigned db) {
    g_db = db;
}

unsigned pace_driver_db(void) {
    return g_db;
}
