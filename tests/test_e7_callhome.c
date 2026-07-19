/**
 * PR-4a/4b: Call Home identity + CLIENT hello → OPEN + subscribe + lab.v1 apply.
 *
 * Lab peer: identity fixture, server-shaped hello, accept create-subscription,
 * send lab_v1_ont_up notification (delimiter framed). Asserts net.pon ONT key
 * and optional WS STATE_CHANGED after coalesce flush.
 *
 * Skips cleanly when EDGEHOST_HAVE_LIBNETCONF is 0.
 */
#include "edge_config.h"
#include "edge_e7_callhome.h"
#include "edge_state.h"
#include "edge_ws.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#if EDGEHOST_HAVE_LIBNETCONF
#include "netconf.h"
#endif

static char *load_file(const char *path, size_t *out_len)
{
    FILE *fp;
    long sz;
    char *buf;
    size_t n;

    fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    sz = ftell(fp);
    if (sz < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[n] = '\0';
    if (out_len) {
        *out_len = n;
    }
    return buf;
}

/** Strip HTML/XML comments so wire payload is compact Calix-shaped preamble. */
static size_t strip_xml_comments(char *s, size_t len)
{
    size_t r = 0, w = 0;
    while (r < len) {
        if (r + 3 < len && s[r] == '<' && s[r + 1] == '!' && s[r + 2] == '-' &&
            s[r + 3] == '-') {
            r += 4;
            while (r + 2 < len &&
                   !(s[r] == '-' && s[r + 1] == '-' && s[r + 2] == '>')) {
                r++;
            }
            if (r + 2 < len) {
                r += 3;
            }
            continue;
        }
        s[w++] = s[r++];
    }
    return w;
}

static void trim_ws(char *s, size_t *len)
{
    size_t n = *len;
    while (n > 0 &&
           (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' ||
            s[n - 1] == '\t')) {
        n--;
    }
    s[n] = '\0';
    *len = n;
}

static uint16_t pick_port(void)
{
    return (uint16_t)(24000 + (getpid() % 2000));
}

#if EDGEHOST_HAVE_LIBNETCONF

typedef struct {
    uint16_t port;
    char    *identity;
    size_t   identity_len;
    char    *notif; /* optional framed notification XML after sub ok */
    size_t   notif_len;
    int      send_sub_reply; /* 1 = accept create-subscription */
    int      hold_open_ms;   /* keep socket open after open (for disconnect) */
    int      ok;
    char     err[128];
} peer_args_t;

/** Extract message-id="N" from an rpc blob; returns 0 if not found. */
static int extract_message_id(const char *buf, size_t len, char *out, size_t out_sz)
{
    const char *p;
    const char *end = buf + len;
    const char *key = "message-id=\"";
    size_t klen = strlen(key);
    size_t i;

    for (p = buf; p + klen < end; p++) {
        if (memcmp(p, key, klen) == 0) {
            p += klen;
            i = 0;
            while (p < end && *p != '"' && i + 1 < out_sz) {
                out[i++] = *p++;
            }
            out[i] = '\0';
            return i > 0 ? 0 : -1;
        }
    }
    return -1;
}

static void *lab_peer_thread(void *arg)
{
    peer_args_t *pa = (peer_args_t *)arg;
    int fd = -1;
    struct sockaddr_in addr;
    netconf_config_t ncfg;
    netconf_ctx_t *server = NULL;
    uint8_t buf[16384];
    size_t n;
    int i;
    int open = 0;
    int sub_done = 0;
    char accum[32768];
    size_t accum_len = 0;

    pa->ok = 0;
    pa->err[0] = '\0';

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(pa->err, sizeof(pa->err), "socket: %s", strerror(errno));
        return NULL;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(pa->port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    for (i = 0; i < 50; i++) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            break;
        }
        usleep(20000);
    }
    if (i >= 50) {
        snprintf(pa->err, sizeof(pa->err), "connect failed: %s",
                 strerror(errno));
        close(fd);
        return NULL;
    }

    /* 1. Calix identity preamble (not NETCONF) */
    if (write(fd, pa->identity, pa->identity_len) < 0) {
        snprintf(pa->err, sizeof(pa->err), "write identity: %s",
                 strerror(errno));
        close(fd);
        return NULL;
    }

    /* 2. Device/server NETCONF SM: receive client hello, send server hello */
    memset(&ncfg, 0, sizeof(ncfg));
    ncfg.event_queue_size = 8;
    ncfg.max_rpc_size = 256 * 1024;
    ncfg.max_output_size = 256 * 1024;
    server = netconf_create_with_config(NETCONF_ROLE_CALLHOME_SERVER, &ncfg);
    if (!server) {
        snprintf(pa->err, sizeof(pa->err), "netconf_create server failed");
        close(fd);
        return NULL;
    }

    for (i = 0; i < 200 && !open; i++) {
        ssize_t rn;
        netconf_event_t ev;

        rn = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (rn > 0) {
            (void)netconf_feed_input(server, buf, (size_t)rn);
            while (netconf_next_event(server, &ev) == 1) {
                if (ev.type == NETCONF_EVENT_HELLO_RECEIVED) {
                    (void)netconf_send_hello(server, NULL, 0);
                }
            }
            /* Stash bytes for post-open RPC (create-subscription) */
            if (accum_len + (size_t)rn < sizeof(accum)) {
                memcpy(accum + accum_len, buf, (size_t)rn);
                accum_len += (size_t)rn;
            }
        }

        for (;;) {
            n = netconf_get_output(server, buf, sizeof(buf));
            if (n == 0) {
                break;
            }
            if (write(fd, buf, n) < 0) {
                snprintf(pa->err, sizeof(pa->err), "write hello: %s",
                         strerror(errno));
                netconf_destroy(server);
                close(fd);
                return NULL;
            }
        }

        if (netconf_current_state(server) == NETCONF_STATE_SESSION_OPEN) {
            open = 1;
            break;
        }
        usleep(10000);
    }

    if (!open) {
        snprintf(pa->err, sizeof(pa->err), "peer did not reach SESSION_OPEN");
        netconf_destroy(server);
        close(fd);
        return NULL;
    }

    /* Server SM no longer needed for raw RPC/notif framing */
    netconf_destroy(server);
    server = NULL;

    if (pa->send_sub_reply) {
        /* Wait for create-subscription RPC; reply <ok/> */
        for (i = 0; i < 200 && !sub_done; i++) {
            ssize_t rn;
            char mid[32];
            char reply[256];
            int rlen;

            rn = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
            if (rn > 0) {
                if (accum_len + (size_t)rn < sizeof(accum)) {
                    memcpy(accum + accum_len, buf, (size_t)rn);
                    accum_len += (size_t)rn;
                }
            }
            if (accum_len > 0 &&
                (strstr(accum, "create-subscription") != NULL ||
                 strstr(accum, "create-subscription") != NULL)) {
                if (extract_message_id(accum, accum_len, mid, sizeof(mid)) !=
                    0) {
                    snprintf(mid, sizeof(mid), "1");
                }
                rlen = snprintf(
                    reply, sizeof(reply),
                    "<rpc-reply message-id=\"%s\" "
                    "xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
                    "<ok/></rpc-reply>]]>]]>",
                    mid);
                if (rlen > 0 && write(fd, reply, (size_t)rlen) < 0) {
                    snprintf(pa->err, sizeof(pa->err), "write sub ok: %s",
                             strerror(errno));
                    close(fd);
                    return NULL;
                }
                sub_done = 1;
                break;
            }
            usleep(10000);
        }
        if (!sub_done) {
            snprintf(pa->err, sizeof(pa->err),
                     "did not receive create-subscription");
            close(fd);
            return NULL;
        }

        /* Send lab.v1 notification if provided */
        if (pa->notif && pa->notif_len > 0) {
            if (write(fd, pa->notif, pa->notif_len) < 0) {
                snprintf(pa->err, sizeof(pa->err), "write notif: %s",
                         strerror(errno));
                close(fd);
                return NULL;
            }
        }
    }

    if (pa->hold_open_ms > 0) {
        usleep((useconds_t)pa->hold_open_ms * 1000u);
    } else {
        usleep(50000);
    }
    close(fd);
    pa->ok = 1;
    return NULL;
}

static void test_profile_and_rss(void)
{
    netconf_config_t cfg;
    size_t est;

    edge_e7_netconf_profile(&cfg);
    assert(cfg.event_queue_size == 8);
    assert(cfg.max_rpc_size == 256 * 1024);
    assert(cfg.max_notification_size == 64 * 1024);
    assert(cfg.max_output_size == 256 * 1024);
    est = edge_e7_session_rss_estimate();
    assert(est > 256 * 1024);
    printf("  PASS: e7 netconf profile + rss estimate (%zu bytes/sess)\n",
           est);
}

static void fill_lab_cfg(edge_config_t *cfg, uint16_t port)
{
    edge_config_defaults(cfg);
    cfg->e7_enabled = 1;
    snprintf(cfg->e7_listen_host, sizeof(cfg->e7_listen_host), "127.0.0.1");
    cfg->e7_listen_port = port;
    snprintf(cfg->e7_transport, sizeof(cfg->e7_transport), "raw");
    cfg->e7_max_sessions = 4;
    cfg->e7_rss_budget_bytes = 268435456;
    cfg->e7_auto_subscribe_unknown = 0;
    cfg->e7_dirty_cap = 64; /* small for tests */
    cfg->e7_shelf_count = 1;
    snprintf(cfg->e7_shelves[0].mac, sizeof(cfg->e7_shelves[0].mac),
             "00:02:5d:d9:21:47");
    cfg->e7_shelves[0].enabled = 1;
    snprintf(cfg->e7_shelves[0].shelf_id, sizeof(cfg->e7_shelves[0].shelf_id),
             "lab-e7-1");
    cfg->state_inventory_enabled = 1;
    cfg->state_inventory_max_keys = 64;
    cfg->state_net_pon_enabled = 1;
    cfg->state_net_pon_max_keys = 64;
}

static char *load_identity_fixture(size_t *out_len)
{
    char *id_raw;
    size_t id_len;

    id_raw = load_file("tests/fixtures/e7/lab_v1_identity.xml", &id_len);
    if (!id_raw) {
        return NULL;
    }
    id_len = strip_xml_comments(id_raw, id_len);
    trim_ws(id_raw, &id_len);
    if (out_len) {
        *out_len = id_len;
    }
    return id_raw;
}

/** Build delimiter-framed notification from fixture (comments stripped). */
static char *load_notif_framed(const char *path, size_t *out_len)
{
    char *raw;
    size_t len;
    char *framed;
    static const char delim[] = "]]>]]>";

    raw = load_file(path, &len);
    if (!raw) {
        return NULL;
    }
    len = strip_xml_comments(raw, len);
    trim_ws(raw, &len);
    framed = (char *)malloc(len + sizeof(delim));
    if (!framed) {
        free(raw);
        return NULL;
    }
    memcpy(framed, raw, len);
    memcpy(framed + len, delim, sizeof(delim) - 1);
    framed[len + sizeof(delim) - 1] = '\0';
    free(raw);
    if (out_len) {
        *out_len = len + sizeof(delim) - 1;
    }
    return framed;
}

static void test_identity_hello_open(void)
{
    edge_config_t cfg;
    edge_state_store_t *store;
    edge_e7_callhome_opts_t opts;
    edge_e7_callhome_t *ch;
    peer_args_t pa;
    pthread_t thr;
    uint16_t port;
    int i;
    char *id_raw;
    size_t id_len;
    char val[512];
    size_t vlen;
    edge_state_err_t e;
    const edge_e7_callhome_stats_t *st;

    port = pick_port();
    fill_lab_cfg(&cfg, port);

    {
        edge_state_config_t sc = edge_state_config_from_edge_config(&cfg);
        store = edge_state_create_with_config(&sc);
    }
    assert(store);
    edge_state_apply_config(store, &cfg);

    memset(&opts, 0, sizeof(opts));
    opts.cfg = &cfg;
    opts.state = store;
    opts.hub = NULL;
    ch = edge_e7_callhome_create(&opts);
    assert(ch);
    assert(edge_e7_callhome_bind(ch) == 0);
    assert(edge_e7_callhome_listen_fd(ch) >= 0);

    id_raw = load_identity_fixture(&id_len);
    assert(id_raw);

    memset(&pa, 0, sizeof(pa));
    pa.port = port;
    pa.identity = id_raw;
    pa.identity_len = id_len;
    pa.send_sub_reply = 0; /* PR-4a path: open only; peer closes before sub */
    assert(pthread_create(&thr, NULL, lab_peer_thread, &pa) == 0);

    for (i = 0; i < 300; i++) {
        edge_e7_callhome_poll(ch, 0);
        if (edge_e7_callhome_open_count(ch) >= 1) {
            break;
        }
        usleep(10000);
    }

    assert(pthread_join(thr, NULL) == 0);
    free(id_raw);

    if (!pa.ok) {
        fprintf(stderr, "lab peer failed: %s\n", pa.err);
    }
    assert(pa.ok);

    /* Peer may have closed; host may still show OPEN until pump sees EOF */
    for (i = 0; i < 50; i++) {
        edge_e7_callhome_poll(ch, 0);
        usleep(5000);
    }

    /* Session may already be disconnected after peer close — either is fine
     * if inventory was written with open at some point. Re-check via stats. */
    st = edge_e7_callhome_stats(ch);
    assert(st);
    assert(st->accepts >= 1);
    assert(st->sessions_opened >= 1);
    assert(st->rejects_bad_identity == 0);
    assert(st->rejects_not_allowlisted == 0);

    e = edge_state_get(store, "inventory", "e7/00-02-5d-d9-21-47/session", val,
                       sizeof(val), &vlen);
    assert(e == EDGE_STATE_OK);
    val[vlen < sizeof(val) ? vlen : sizeof(val) - 1] = '\0';
    assert(strstr(val, "00:02:5d:d9:21:47") != NULL);
    /* open or disconnected after peer exit */
    assert(strstr(val, "open") != NULL || strstr(val, "disconnected") != NULL);
    assert(strstr(val, "071904926728") != NULL);

    edge_e7_callhome_destroy(ch);
    edge_state_destroy(store);
    printf("  PASS: identity + CLIENT hello → OPEN + inventory session\n");
}

static void test_subscribe_apply_ont_up(void)
{
    edge_config_t cfg;
    edge_state_store_t *store;
    edge_ws_hub_t *hub;
    edge_e7_callhome_opts_t opts;
    edge_e7_callhome_t *ch;
    peer_args_t pa;
    pthread_t thr;
    uint16_t port;
    int i;
    char *id_raw;
    size_t id_len;
    char *notif;
    size_t notif_len;
    char val[512];
    size_t vlen;
    edge_state_err_t e;
    const edge_e7_callhome_stats_t *st;
    char msg[EDGE_WS_MSG_MAX];
    size_t mlen;
    uint64_t t0 = 1000;
    int saw_ws = 0;

    port = (uint16_t)(pick_port() + 1);
    fill_lab_cfg(&cfg, port);

    {
        edge_state_config_t sc = edge_state_config_from_edge_config(&cfg);
        store = edge_state_create_with_config(&sc);
    }
    assert(store);
    edge_state_apply_config(store, &cfg);

    hub = edge_ws_hub_create(4);
    assert(hub);
    assert(edge_ws_hub_subscribe(hub, 0) == 0);

    memset(&opts, 0, sizeof(opts));
    opts.cfg = &cfg;
    opts.state = store;
    opts.hub = hub;
    ch = edge_e7_callhome_create(&opts);
    assert(ch);
    assert(edge_e7_callhome_bind(ch) == 0);

    id_raw = load_identity_fixture(&id_len);
    assert(id_raw);
    notif = load_notif_framed("tests/fixtures/e7/lab_v1_ont_up.xml", &notif_len);
    assert(notif);

    memset(&pa, 0, sizeof(pa));
    pa.port = port;
    pa.identity = id_raw;
    pa.identity_len = id_len;
    pa.notif = notif;
    pa.notif_len = notif_len;
    pa.send_sub_reply = 1;
    pa.hold_open_ms = 200;
    assert(pthread_create(&thr, NULL, lab_peer_thread, &pa) == 0);

    for (i = 0; i < 400; i++) {
        edge_e7_callhome_poll(ch, t0 + (uint64_t)i * 10u);
        st = edge_e7_callhome_stats(ch);
        if (st && st->notifications >= 1 && st->subscriptions_ok >= 1) {
            /* advance past coalesce interval to force flush */
            edge_e7_callhome_on_tick(ch, t0 + (uint64_t)i * 10u +
                                             EDGE_E7_COALESCE_MS + 1u);
            break;
        }
        usleep(10000);
    }

    assert(pthread_join(thr, NULL) == 0);
    free(id_raw);
    free(notif);

    if (!pa.ok) {
        fprintf(stderr, "lab peer (sub) failed: %s\n", pa.err);
    }
    assert(pa.ok);

    /* Final flush in case notification arrived late */
    for (i = 0; i < 30; i++) {
        edge_e7_callhome_on_tick(ch, t0 + 5000u + (uint64_t)i * EDGE_E7_COALESCE_MS);
        e = edge_state_get(store, "net.pon",
                           "e7/00-02-5d-d9-21-47/ont/1-1-3-12", val, sizeof(val),
                           &vlen);
        if (e == EDGE_STATE_OK) {
            break;
        }
        usleep(10000);
    }

    e = edge_state_get(store, "net.pon", "e7/00-02-5d-d9-21-47/ont/1-1-3-12",
                       val, sizeof(val), &vlen);
    assert(e == EDGE_STATE_OK);
    val[vlen < sizeof(val) ? vlen : sizeof(val) - 1] = '\0';
    assert(strstr(val, "\"oper_state\":\"up\"") != NULL ||
           strstr(val, "oper_state") != NULL);
    assert(strstr(val, "1/1/3/12") != NULL);
    assert(strstr(val, "lab.v1") != NULL);

    st = edge_e7_callhome_stats(ch);
    assert(st);
    assert(st->sessions_opened >= 1);
    assert(st->subscriptions_ok >= 1);
    assert(st->notifications >= 1);
    assert(st->state_puts >= 1);

    /* Drain hub: session open (immediate) and/or ONT coalesce flush */
    for (i = 0; i < 16; i++) {
        if (edge_ws_hub_take_pending(hub, 0, msg, sizeof(msg), &mlen) != 1) {
            break;
        }
        msg[mlen < sizeof(msg) ? mlen : sizeof(msg) - 1] = '\0';
        if (strstr(msg, "STATE_CHANGED") != NULL ||
            strstr(msg, "state_changed") != NULL ||
            strstr(msg, "net.pon") != NULL ||
            strstr(msg, "inventory") != NULL) {
            saw_ws = 1;
        }
        if (strstr(msg, "net.pon") != NULL ||
            strstr(msg, "1-1-3-12") != NULL) {
            saw_ws = 1;
            break;
        }
    }
    assert(saw_ws);

    edge_e7_callhome_destroy(ch);
    edge_ws_hub_destroy(hub);
    edge_state_destroy(store);
    printf("  PASS: subscribe + lab.v1 ont_up → net.pon + STATE_CHANGED\n");
}

static void test_reject_unknown_mac(void)
{
    edge_config_t cfg;
    edge_e7_callhome_opts_t opts;
    edge_e7_callhome_t *ch;
    int fd;
    struct sockaddr_in addr;
    uint16_t port;
    const char *id =
        "<version>1</version><identity><mac>aa:bb:cc:dd:ee:ff</mac>"
        "<serial-number>x</serial-number><model-name>E7</model-name>"
        "<source-ip>10.0.0.1</source-ip></identity>";
    int i;
    const edge_e7_callhome_stats_t *st;

    port = (uint16_t)(pick_port() + 2);
    edge_config_defaults(&cfg);
    cfg.e7_enabled = 1;
    snprintf(cfg.e7_listen_host, sizeof(cfg.e7_listen_host), "127.0.0.1");
    cfg.e7_listen_port = port;
    cfg.e7_max_sessions = 4;
    cfg.e7_rss_budget_bytes = 268435456;
    cfg.e7_auto_subscribe_unknown = 0;
    cfg.e7_shelf_count = 1;
    snprintf(cfg.e7_shelves[0].mac, sizeof(cfg.e7_shelves[0].mac),
             "00:02:5d:d9:21:47");
    cfg.e7_shelves[0].enabled = 1;

    memset(&opts, 0, sizeof(opts));
    opts.cfg = &cfg;
    ch = edge_e7_callhome_create(&opts);
    assert(ch);
    assert(edge_e7_callhome_bind(ch) == 0);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    for (i = 0; i < 50; i++) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            break;
        }
        usleep(10000);
    }
    assert(i < 50);
    assert(write(fd, id, strlen(id)) > 0);

    for (i = 0; i < 50; i++) {
        edge_e7_callhome_poll(ch, 0);
        st = edge_e7_callhome_stats(ch);
        if (st && st->rejects_not_allowlisted >= 1) {
            break;
        }
        usleep(10000);
    }
    close(fd);

    st = edge_e7_callhome_stats(ch);
    assert(st && st->rejects_not_allowlisted >= 1);
    assert(edge_e7_callhome_open_count(ch) == 0);

    edge_e7_callhome_destroy(ch);
    printf("  PASS: reject unknown MAC (not allowlisted)\n");
}

static void test_status_json_and_allowlist(void)
{
    edge_config_t cfg;
    edge_e7_callhome_opts_t opts;
    edge_e7_callhome_t *ch;
    edge_state_store_t *st;
    char buf[4096];
    int n;
    char val[512];
    size_t vlen = 0;

    edge_config_defaults(&cfg);
    cfg.e7_enabled = 1;
    cfg.e7_max_sessions = 4;
    cfg.e7_rss_budget_bytes = 256u * 1024u * 1024u;
    cfg.e7_listen_host[0] = '\0';
    snprintf(cfg.e7_listen_host, sizeof(cfg.e7_listen_host), "127.0.0.1");
    cfg.e7_listen_port = 0; /* bind not required for this test */
    cfg.e7_shelf_count = 1;
    snprintf(cfg.e7_shelves[0].mac, sizeof(cfg.e7_shelves[0].mac),
             "00:02:5d:d9:21:47");
    snprintf(cfg.e7_shelves[0].shelf_id, sizeof(cfg.e7_shelves[0].shelf_id),
             "yaml-seed");
    cfg.e7_shelves[0].enabled = 1;

    st = edge_state_create();
    assert(st);
    (void)edge_state_ns_set_enabled(st, "inventory", 1);
    (void)edge_state_ns_set_enabled(st, "net.pon", 1);

    memset(&opts, 0, sizeof(opts));
    opts.cfg = &cfg;
    opts.state = st;
    opts.hub = NULL;
    ch = edge_e7_callhome_create(&opts);
    assert(ch);

    n = edge_e7_callhome_status_json(ch, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "\"e7_accepts\"") != NULL);
    assert(strstr(buf, "\"e7_sessions_open\"") != NULL);
    assert(strstr(buf, "\"e7_rss_estimate\"") != NULL);
    assert(strstr(buf, "\"runtime_shelves\":1") != NULL);

    n = edge_e7_callhome_shelves_json(ch, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "00:02:5d:d9:21:47") != NULL);
    assert(strstr(buf, "allowlist") != NULL || strstr(buf, "YAML") != NULL);

    /* REST upsert another MAC (runtime-only) */
    assert(edge_e7_callhome_allowlist_upsert(ch, "11-22-33-44-55-66", "lab-2",
                                             1) == 0);
    n = edge_e7_callhome_shelf_json(ch, "11:22:33:44:55:66", buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "lab-2") != NULL);
    assert(strstr(buf, "\"configured\":true") != NULL);

    assert(edge_state_get(st, "inventory", "e7/11-22-33-44-55-66/config", val,
                          sizeof(val), &vlen) == EDGE_STATE_OK);
    assert(strstr(val, "allowlist_path") != NULL ||
           strstr(val, "YAML") != NULL);

    assert(edge_e7_callhome_allowlist_delete(ch, "11:22:33:44:55:66") == 0);
    assert(edge_e7_callhome_shelf_json(ch, "11:22:33:44:55:66", buf,
                                       sizeof(buf)) == -2);

    /* Command with no session → 409 */
    {
        char cmd_id[32];
        int http_st = 0;
        assert(edge_e7_callhome_command_submit(ch, "00:02:5d:d9:21:47",
                                               "<get/>", 6, NULL, cmd_id,
                                               sizeof(cmd_id), &http_st) != 0);
        assert(http_st == 409);
    }

    n = edge_e7_callhome_onts_json(ch, "00:02:5d:d9:21:47", NULL, 0, buf,
                                   sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "\"onts\":[") != NULL);

    edge_e7_callhome_destroy(ch);
    edge_state_destroy(st);
    printf("  PASS: status JSON + runtime allowlist + empty onts\n");
}

/** K15: reload_policy merge — YAML wins for listed MACs; runtime-only kept. */
static void test_apply_config_merge(void)
{
    edge_config_t cfg;
    edge_config_t next;
    edge_e7_callhome_opts_t opts;
    edge_e7_callhome_t *ch;
    edge_state_store_t *st;
    char buf[4096];
    int n;

    edge_config_defaults(&cfg);
    cfg.e7_enabled = 1;
    cfg.e7_max_sessions = 4;
    cfg.e7_rss_budget_bytes = 256u * 1024u * 1024u;
    snprintf(cfg.e7_listen_host, sizeof(cfg.e7_listen_host), "127.0.0.1");
    cfg.e7_listen_port = 4334;
    snprintf(cfg.e7_reload_policy, sizeof(cfg.e7_reload_policy), "merge");
    cfg.e7_shelf_count = 1;
    snprintf(cfg.e7_shelves[0].mac, sizeof(cfg.e7_shelves[0].mac),
             "00:02:5d:d9:21:47");
    snprintf(cfg.e7_shelves[0].shelf_id, sizeof(cfg.e7_shelves[0].shelf_id),
             "yaml-seed");
    cfg.e7_shelves[0].enabled = 1;

    st = edge_state_create();
    assert(st);
    (void)edge_state_ns_set_enabled(st, "inventory", 1);
    (void)edge_state_ns_set_enabled(st, "net.pon", 1);

    memset(&opts, 0, sizeof(opts));
    opts.cfg = &cfg;
    opts.state = st;
    ch = edge_e7_callhome_create(&opts);
    assert(ch);

    /* Runtime-only shelf (not in next YAML). */
    assert(edge_e7_callhome_allowlist_upsert(ch, "aa:bb:cc:dd:ee:ff",
                                             "runtime-only", 1) == 0);

    /* Next YAML: update seed label/enabled, add new shelf, omit runtime-only. */
    next = cfg;
    snprintf(next.e7_shelves[0].shelf_id, sizeof(next.e7_shelves[0].shelf_id),
             "yaml-updated");
    next.e7_shelves[0].enabled = 0;
    next.e7_shelf_count = 2;
    snprintf(next.e7_shelves[1].mac, sizeof(next.e7_shelves[1].mac),
             "11:22:33:44:55:66");
    snprintf(next.e7_shelves[1].shelf_id, sizeof(next.e7_shelves[1].shelf_id),
             "yaml-new");
    next.e7_shelves[1].enabled = 1;

    assert(edge_e7_callhome_apply_config(ch, &next) == 0);

    n = edge_e7_callhome_shelf_json(ch, "00:02:5d:d9:21:47", buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "yaml-updated") != NULL);
    assert(strstr(buf, "\"enabled\":false") != NULL ||
           strstr(buf, "\"enabled\": false") != NULL);

    n = edge_e7_callhome_shelf_json(ch, "11:22:33:44:55:66", buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "yaml-new") != NULL);

    n = edge_e7_callhome_shelf_json(ch, "aa:bb:cc:dd:ee:ff", buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "runtime-only") != NULL);

    n = edge_e7_callhome_status_json(ch, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "\"runtime_shelves\":3") != NULL);

    /* replace_all drops runtime-only */
    snprintf(next.e7_reload_policy, sizeof(next.e7_reload_policy),
             "replace_all");
    next.e7_shelf_count = 1;
    assert(edge_e7_callhome_apply_config(ch, &next) == 0);
    assert(edge_e7_callhome_shelf_json(ch, "aa:bb:cc:dd:ee:ff", buf,
                                       sizeof(buf)) == -2);
    n = edge_e7_callhome_status_json(ch, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "\"runtime_shelves\":1") != NULL);

    /* SSH transport: create (+ bind) when libassh available; else hard fail. */
    {
        edge_config_t ssh_cfg;
        edge_e7_callhome_opts_t sopts;
        edge_e7_callhome_t *ssh_ch;
        edge_config_defaults(&ssh_cfg);
        ssh_cfg.e7_enabled = 1;
        ssh_cfg.e7_max_sessions = 4;
        ssh_cfg.e7_rss_budget_bytes = 256u * 1024u * 1024u;
        snprintf(ssh_cfg.e7_listen_host, sizeof(ssh_cfg.e7_listen_host),
                 "127.0.0.1");
        ssh_cfg.e7_listen_port = pick_port();
        snprintf(ssh_cfg.e7_transport, sizeof(ssh_cfg.e7_transport), "ssh");
        snprintf(ssh_cfg.e7_ssh_password, sizeof(ssh_cfg.e7_ssh_password),
                 "lab");
        snprintf(ssh_cfg.e7_ssh_username, sizeof(ssh_cfg.e7_ssh_username),
                 "netconf");
        memset(&sopts, 0, sizeof(sopts));
        sopts.cfg = &ssh_cfg;
        sopts.state = st;
        ssh_ch = edge_e7_callhome_create(&sopts);
#if EDGEHOST_E7_SSH_AVAILABLE
        assert(ssh_ch != NULL);
        assert(edge_e7_callhome_bind(ssh_ch) == 0);
        assert(edge_e7_callhome_listen_fd(ssh_ch) >= 0);
        /* Profile remains raw-shaped until session_start wires CALLHOME. */
        {
            netconf_config_t ncfg;
            edge_e7_netconf_profile(&ncfg);
            assert(ncfg.ssh_mode == NETCONF_SSH_OFF || ncfg.ssh_mode == 0);
            assert(ncfg.event_queue_size == 8);
        }
        edge_e7_callhome_destroy(ssh_ch);
#else
        assert(ssh_ch == NULL);
#endif
    }

    edge_e7_callhome_destroy(ch);
    edge_state_destroy(st);
    printf("  PASS: apply_config merge/replace_all + ssh create\n");
}

static void test_allowlist_file_roundtrip(void)
{
    edge_state_store_t *st;
    edge_config_t cfg;
    edge_e7_callhome_opts_t opts;
    edge_e7_callhome_t *ch;
    edge_e7_callhome_t *ch2;
    char path[] = "/tmp/edgehost-e7-allowlist-test.XXXXXX";
    char buf[2048];
    int fd;
    int n;

    fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    (void)unlink(path); /* create writes file; start empty */

    st = edge_state_create();
    assert(st);
    (void)edge_state_ns_set_enabled(st, "inventory", 1);
    (void)edge_state_ns_set_enabled(st, "net.pon", 1);

    edge_config_defaults(&cfg);
    cfg.e7_enabled = 1;
    cfg.e7_max_sessions = 4;
    cfg.e7_rss_budget_bytes = 256u * 1024u * 1024u;
    snprintf(cfg.e7_listen_host, sizeof(cfg.e7_listen_host), "127.0.0.1");
    cfg.e7_listen_port = 4334;
    snprintf(cfg.e7_allowlist_path, sizeof(cfg.e7_allowlist_path), "%s", path);

    memset(&opts, 0, sizeof(opts));
    opts.cfg = &cfg;
    opts.state = st;
    ch = edge_e7_callhome_create(&opts);
    assert(ch);
    assert(edge_e7_callhome_allowlist_upsert(ch, "00:02:5d:aa:bb:cc", "file-lab",
                                             1) == 0);
    edge_e7_callhome_destroy(ch);

    /* New instance should load durable shelf without YAML entry */
    ch2 = edge_e7_callhome_create(&opts);
    assert(ch2);
    n = edge_e7_callhome_shelf_json(ch2, "00:02:5d:aa:bb:cc", buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "file-lab") != NULL);
    edge_e7_callhome_destroy(ch2);
    edge_state_destroy(st);
    (void)unlink(path);
    printf("  PASS: allowlist file durability roundtrip\n");
}

#endif /* EDGEHOST_HAVE_LIBNETCONF */

int main(void)
{
    printf("test_e7_callhome:\n");
#if !EDGEHOST_HAVE_LIBNETCONF
    printf("  SKIP: EDGEHOST_HAVE_LIBNETCONF=0 (build sibling libnetconf)\n");
    return 0;
#else
    test_profile_and_rss();
    test_identity_hello_open();
    test_subscribe_apply_ont_up();
    test_reject_unknown_mac();
    test_status_json_and_allowlist();
    test_apply_config_merge();
    test_allowlist_file_roundtrip();
    printf("All e7_callhome tests passed.\n");
    return 0;
#endif
}
