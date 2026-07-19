/**
 * @file main.c
 * @brief edgehost process entry — config, auth, plugins, io_uring (P1.8b).
 */

#include "edge_auth_host.h"
#include "edge_config.h"
#include "edge_config_hup.h"
#include "edge_e7_callhome.h"
#include "edge_iouring.h"
#include "edge_openai_proxy.h"
#include "edge_plugin_host.h"
#include "edge_slack_plugin.h"
#include "edge_state.h"
#include "edge_teams_plugin.h"
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
    edge_plugin_host_t *ph = NULL;
    edge_plugin_t openai_plugin;
    edge_plugin_t slack_plugin;
    edge_plugin_t teams_plugin;
    edge_openai_proxy_config_t openai_cfg;
    edge_openai_proxy_config_t openai_cfg_store;
    edge_slack_config_t slack_cfg, slack_store;
    edge_teams_config_t teams_cfg, teams_store;
    edge_state_store_t *store = NULL;
    edge_e7_callhome_t *e7 = NULL;
    char service_key_buf[256];
    const char *service_key = NULL;
    edge_event_t ev;
    char err[160];
    int rc;
    int opt;

    memset(&openai_plugin, 0, sizeof(openai_plugin));
    memset(&slack_plugin, 0, sizeof(slack_plugin));
    memset(&teams_plugin, 0, sizeof(teams_plugin));
    service_key_buf[0] = '\0';

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

    {
        const edge_config_t *ac = edgecore_config(core);
        int need_ph =
            ac->openai_enabled || ac->slack_enabled || ac->teams_enabled;
        int need_store = need_ph || ac->e7_enabled;
        if (need_store) {
            edge_state_config_t sc = edge_state_config_from_edge_config(ac);
            store = edge_state_create_with_config(&sc);
            if (store) {
                edge_state_apply_config(store, ac);
            }
        }
        if (need_ph) {
            edge_plugin_host_config_t phc;
            memset(&phc, 0, sizeof(phc));
            phc.max_pending = ac->http_max_pending_outbound;
            phc.state = store;
            ph = edge_plugin_host_create(&phc);
            if (!ph) {
                fprintf(stderr, "edgehost: plugin host create failed\n");
                edgecore_destroy(core);
                edge_state_destroy(store);
                return 1;
            }
        }
        if (ac->openai_enabled && ph) {
            edge_openai_proxy_config_defaults(&openai_cfg);
            openai_cfg.enabled = 1;
            snprintf(openai_cfg.upstream, sizeof(openai_cfg.upstream), "%s",
                     ac->openai_upstream);
            snprintf(openai_cfg.upstream_addr, sizeof(openai_cfg.upstream_addr),
                     "%s", ac->openai_upstream_addr);
            snprintf(openai_cfg.upstream_host, sizeof(openai_cfg.upstream_host),
                     "%s", ac->openai_upstream_host);
            snprintf(openai_cfg.api_key_env, sizeof(openai_cfg.api_key_env), "%s",
                     ac->openai_api_key_env);
            snprintf(openai_cfg.service_api_key_env,
                     sizeof(openai_cfg.service_api_key_env), "%s",
                     ac->openai_service_key_env);
            openai_cfg.timeout_ms = ac->openai_timeout_ms;
            openai_cfg.rate_limit_rpm = ac->openai_rate_limit_rpm;
            openai_cfg.max_concurrent_per_principal = ac->openai_max_concurrent;
            if (edge_openai_proxy_init_plugin(&openai_plugin, &openai_cfg_store,
                                              &openai_cfg) != 0) {
                fprintf(stderr,
                        "edgehost: openai_proxy init failed (set %s)\n",
                        openai_cfg.api_key_env);
                edge_plugin_host_destroy(ph);
                edge_state_destroy(store);
                edgecore_destroy(core);
                return 1;
            }
            if (edge_plugin_host_register(ph, &openai_plugin, NULL) != 0 ||
                edge_plugin_host_add_route(ph, "/v1", &openai_plugin) != 0) {
                fprintf(stderr, "edgehost: openai_proxy register failed\n");
                edge_plugin_host_destroy(ph);
                edge_state_destroy(store);
                edgecore_destroy(core);
                return 1;
            }
            {
                const char *sk = getenv(openai_cfg.service_api_key_env[0]
                                            ? openai_cfg.service_api_key_env
                                            : "EDGEHOST_OPENAI_SERVICE_KEY");
                if (sk && sk[0] && strlen(sk) < sizeof(service_key_buf)) {
                    snprintf(service_key_buf, sizeof(service_key_buf), "%s", sk);
                    service_key = service_key_buf;
                }
            }
            fprintf(stderr, "edgehost: openai_proxy enabled upstream=%s\n",
                    openai_cfg.upstream);
        }
        if (ac->slack_enabled && ph) {
            edge_slack_config_defaults(&slack_cfg);
            slack_cfg.enabled = 1;
            if (edge_slack_init_plugin(&slack_plugin, &slack_store, &slack_cfg) !=
                    0 ||
                edge_plugin_host_register(ph, &slack_plugin, NULL) != 0) {
                fprintf(stderr, "edgehost: slack stub register failed\n");
                edge_plugin_host_destroy(ph);
                edge_state_destroy(store);
                edgecore_destroy(core);
                return 1;
            }
            fprintf(stderr, "edgehost: slack plugin stub registered\n");
        }
        if (ac->teams_enabled && ph) {
            edge_teams_config_defaults(&teams_cfg);
            teams_cfg.enabled = 1;
            if (edge_teams_init_plugin(&teams_plugin, &teams_store, &teams_cfg) !=
                    0 ||
                edge_plugin_host_register(ph, &teams_plugin, NULL) != 0) {
                fprintf(stderr, "edgehost: teams stub register failed\n");
                edge_plugin_host_destroy(ph);
                edge_state_destroy(store);
                edgecore_destroy(core);
                return 1;
            }
            fprintf(stderr, "edgehost: teams plugin stub registered\n");
        }
        if (ac->tls_cert[0] && ac->tls_key[0]) {
            fprintf(stderr, "edgehost: TLS server cert=%s\n", ac->tls_cert);
        }
        if (ac->e7_enabled) {
            edge_e7_callhome_opts_t eopts;
            memset(&eopts, 0, sizeof(eopts));
            eopts.cfg = ac;
            eopts.state = store;
            /* Hub is created inside iouring_run; attach via set_hub there. */
            eopts.hub = NULL;
            e7 = edge_e7_callhome_create(&eopts);
            if (!e7) {
#if EDGEHOST_HAVE_LIBNETCONF
                fprintf(stderr,
                        "edgehost: e7_callhome create failed "
                        "(check max_sessions/rss_budget)\n");
                if (ph) {
                    edge_plugin_host_destroy(ph);
                }
                edge_state_destroy(store);
                edgecore_destroy(core);
                return 1;
#else
                fprintf(stderr,
                        "edgehost: e7_callhome enabled but libnetconf not "
                        "linked; Call Home listen skipped\n");
#endif
            } else {
                fprintf(stderr,
                        "edgehost: e7_callhome enabled transport=%s port=%u\n",
                        ac->e7_transport[0] ? ac->e7_transport : "raw",
                        (unsigned)ac->e7_listen_port);
            }
        }
    }

    edge_iouring_opts_defaults(&iopts);
    iopts.stop = &g_stop;
    iopts.auth = &auth;
    iopts.plugins = ph;
    iopts.service_api_key = service_key;
    iopts.e7 = e7;
    if (store) {
        iopts.state = store;
    }
    if (once) {
        iopts.max_accepts = 1;
    }

    /* HUP reloads config values for next bind only after restart in P1.4a;
     * live rebind lands later. Still drain flag so tests of install work. */
    (void)edgehost_hup_take();

    rc = edge_iouring_run(edgecore_config(core), &iopts);
    if (e7) {
        edge_e7_callhome_destroy(e7);
    }
    if (ph) {
        edge_plugin_host_destroy(ph);
    }
    if (store) {
        edge_state_destroy(store);
    }
    edgecore_destroy(core);
    return rc;
}
