/**
 * P1.0: compile-time smoke that sibling headers resolve via Find modules.
 * Does not call into library .c (no link to sibling static libs required).
 */
#include <stdio.h>

#if defined(EDGEHOST_HAVE_SHAGGY)
#include "http1.h"
#endif
#if defined(EDGEHOST_HAVE_LIBYAML)
#include "yaml.h"
#endif
#if defined(EDGEHOST_HAVE_LIBREST)
#include "rest.h"
#endif

int main(void)
{
    int n = 0;
#if defined(EDGEHOST_HAVE_SHAGGY)
    n++;
    (void)sizeof(http1_config_t);
#endif
#if defined(EDGEHOST_HAVE_LIBYAML)
    n++;
    (void)sizeof(yaml_config_t);
#endif
#if defined(EDGEHOST_HAVE_LIBREST)
    n++;
    (void)sizeof(rest_config_t);
#endif
    printf("edgehost_deps_smoke: %d phase-1 header package(s) visible\n", n);
    if (n < 1) {
        fprintf(stderr, "no sibling headers found — configure with sibling checkouts\n");
        return 1;
    }
    return 0;
}
