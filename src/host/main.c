/**
 * @file main.c
 * @brief edgehost process entry — config, auth, io_uring HTTP/WS (P1.7c).
 */

#include "edge_auth_host.h"
#include "edge_config.h"
#include "edge_config_hup.h"
#include "edge_iouring.h"
#include "edgecore.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t g_stop;

static void on_stop(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [--config PATH] [--host HOST] [--port N] [--once]\n"
            "  --config   YAML path (optional; defaults used if omitted)\n"
            "  --host     override listen.host\n"
            "  --port     override listen.port\n"
            "  --once     accept one connection then exit (tests)\n",
            argv0);
}

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    const char *host_override = NULL;
    int port_override = -1;
    int once = 0;
    edge_config_t cfg;
    edgecore_t *core;
    edge_iouring_opts_t iopts;
    edge_auth_ctx_t auth;
    edge_event_t ev;
    char err[160];
    int rc;
    int opt;

    static struct option long_opts[] = {
        {"config", required_argument, 0, 'c'},
        {"host", required_argument, 0, 'H'},
        {"port", required_argument, 0, 'p'},
        {"once", no_argument, 0, '1'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "c:H:p:1h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'c':
            config_path = optarg;
            break;
        case 'H':
            host_override = optarg;
            break;
        case 'p':
            port_override = atoi(optarg);
            break;
        case '1':
            once = 1;
            break;
        case 'h':
        default:
            usage(argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    edge_config_defaults(&cfg);
    if (config_path) {
        if (edge_config_load_yaml_path(config_path, &cfg, err, sizeof(err)) !=
            0) {
            fprintf(stderr, "edgehost: config load failed: %s\n", err);
            return 1;
        }
    }
    if (host_override) {
        if (strlen(host_override) >= sizeof(cfg.listen_host)) {
            fprintf(stderr, "edgehost: host too long\n");
            return 1;
        }
        snprintf(cfg.listen_host, sizeof(cfg.listen_host), "%s", host_override);
    }
    if (port_override > 0 && port_override <= 65535) {
        cfg.listen_port = (uint16_t)port_override;
    }

    core = edgecore_create();
    if (!core) {
        fprintf(stderr, "edgehost: edgecore_create failed\n");
        return 1;
    }
    if (edgecore_apply_config(core, &cfg) != 0) {
        fprintf(stderr, "edgehost: apply_config rejected\n");
        while (edgecore_next_event(core, &ev) == 1) {
            fprintf(stderr, "  event %s: %s\n",
                    edgecore_event_type_name(ev.type), ev.reason);
        }
        edgecore_destroy(core);
        return 1;
    }
    while (edgecore_next_event(core, &ev) == 1) {
        /* drain CONFIG_APPLIED */
    }

    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_stop;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }
    edgehost_hup_install();

    if (edge_auth_ctx_from_config(edgecore_config(core), &auth, err,
                                  sizeof(err)) != 0) {
        fprintf(stderr, "edgehost: auth init failed: %s\n", err);
        edgecore_destroy(core);
        return 1;
    }
    if (auth.mode == EDGE_AUTH_MODE_LAB_PASSWORD) {
        fprintf(stderr,
                "edgehost: auth mode=lab_password (POST /auth/lab-login)\n");
    } else if (auth.mode == EDGE_AUTH_MODE_PROXY_HEADERS) {
        fprintf(stderr,
                "edgehost: auth mode=proxy_headers (X-User + X-Auth-Signature)\n");
    }

    edge_iouring_opts_defaults(&iopts);
    iopts.stop = &g_stop;
    iopts.auth = &auth;
    if (once) {
        iopts.max_accepts = 1;
    }

    /* HUP reloads config values for next bind only after restart in P1.4a;
     * live rebind lands later. Still drain flag so tests of install work. */
    (void)edgehost_hup_take();

    rc = edge_iouring_run(edgecore_config(core), &iopts);
    edgecore_destroy(core);
    return rc;
}
