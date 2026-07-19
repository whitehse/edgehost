/**
 * @file config_hup.c
 * @brief SIGHUP → flag → shadow reload → edgecore_apply_config (ADR-005).
 */

#include "edge_config_hup.h"

#include "edge_config.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t g_edgehost_hup;

static void edgehost_on_hup(int sig)
{
    (void)sig;
    g_edgehost_hup = 1;
}

void edgehost_hup_install(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = edgehost_on_hup;
    sigemptyset(&sa.sa_mask);
    /* Do not set SA_RESTART — host loop should notice HUP promptly. */
    sa.sa_flags = 0;
    (void)sigaction(SIGHUP, &sa, NULL);
}

int edgehost_hup_take(void)
{
    if (g_edgehost_hup) {
        g_edgehost_hup = 0;
        return 1;
    }
    return 0;
}

int edgehost_reload_config(edgecore_t *core, const char *path, char *err,
                           size_t err_len)
{
    edge_config_t shadow;
    char local_err[160];

    if (!core || !path) {
        if (err && err_len) {
            snprintf(err, err_len, "null args");
        }
        return -1;
    }

    if (edge_config_load_yaml_path(path, &shadow, local_err,
                                   sizeof(local_err)) != 0) {
        if (err && err_len) {
            snprintf(err, err_len, "%s", local_err);
        }
        (void)edgecore_notify_config_rejected(core, local_err);
        return -1;
    }

    if (edgecore_apply_config(core, &shadow) != 0) {
        if (err && err_len) {
            snprintf(err, err_len, "apply_config rejected");
        }
        return -1;
    }
    if (err && err_len) {
        err[0] = '\0';
    }
    return 0;
}
