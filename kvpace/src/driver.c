/* driver.c - driver registry. */
#include <string.h>

#include "driver.h"

const pace_driver *pace_driver_find(const char *name) {
    static const pace_driver *const drivers[] = {
        &pace_driver_resp,
        &pace_driver_memcached,
#ifdef PACE_HAVE_NATIVE
        &pace_driver_native,
#endif
    };
    for (size_t i = 0; i < sizeof(drivers) / sizeof(drivers[0]); i++) {
        if (strcmp(drivers[i]->name, name) == 0) {
            return drivers[i];
        }
    }
    return NULL;
}
