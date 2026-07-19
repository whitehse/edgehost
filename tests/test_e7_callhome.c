/**
 * PR-4a: Call Home listen + identity preamble + raw CLIENT hello → OPEN.
 *
 * Spawns a lab peer that connects, sends lab_v1_identity.xml, then exchanges
 * delimiter-framed NETCONF hellos (server-shaped with session-id). Asserts
 * host reaches SESSION_OPEN and inventory session key is written.
 *
 * Skips cleanly when EDGEHOST_HAVE_LIBNETCONF is 0.
 */
#include "edge_config.h"
#include "edge_e7_callhome.h"
#include "edge_state.h"

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
        /* skip leading whitespace between tags only when at start / after > */
        s[w++] = s[r++];
    }
    return w;
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
    int      ok;
    char     err[128];
} peer_args_t;

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

        /* Read host → feed server */
        rn = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (rn > 0) {
            (void)netconf_feed_input(server, buf, (size_t)rn);
            while (netconf_next_event(server, &ev) == 1) {
                if (ev.type == NETCONF_EVENT_HELLO_RECEIVED) {
                    (void)netconf_send_hello(server, NULL, 0);
                }
            }
        }

        /* Drain server output → host */
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

    /* Hold briefly so host can process peer hello */
    usleep(50000);
    netconf_destroy(server);
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
    edge_config_defaults(&cfg);
    cfg.e7_enabled = 1;
    snprintf(cfg.e7_listen_host, sizeof(cfg.e7_listen_host), "127.0.0.1");
    cfg.e7_listen_port = port;
    snprintf(cfg.e7_transport, sizeof(cfg.e7_transport), "raw");
    cfg.e7_max_sessions = 4;
    cfg.e7_rss_budget_bytes = 268435456;
    cfg.e7_auto_subscribe_unknown = 0;
    cfg.e7_shelf_count = 1;
    snprintf(cfg.e7_shelves[0].mac, sizeof(cfg.e7_shelves[0].mac),
             "00:02:5d:d9:21:47");
    cfg.e7_shelves[0].enabled = 1;
    snprintf(cfg.e7_shelves[0].shelf_id, sizeof(cfg.e7_shelves[0].shelf_id),
             "lab-e7-1");
    cfg.state_inventory_enabled = 1;
    cfg.state_inventory_max_keys = 64;
    cfg.state_net_pon_enabled = 1;

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

    id_raw = load_file("tests/fixtures/e7/lab_v1_identity.xml", &id_len);
    assert(id_raw);
    id_len = strip_xml_comments(id_raw, id_len);
    /* trim whitespace */
    while (id_len > 0 &&
           (id_raw[id_len - 1] == '\n' || id_raw[id_len - 1] == '\r' ||
            id_raw[id_len - 1] == ' ')) {
        id_len--;
    }
    id_raw[id_len] = '\0';

    memset(&pa, 0, sizeof(pa));
    pa.port = port;
    pa.identity = id_raw;
    pa.identity_len = id_len;
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

    assert(edge_e7_callhome_open_count(ch) >= 1);
    assert(edge_e7_callhome_session_state_by_mac(ch, "00:02:5d:d9:21:47") ==
           EDGE_E7_SESS_OPEN);

    e = edge_state_get(store, "inventory", "e7/00-02-5d-d9-21-47/session", val,
                       sizeof(val), &vlen);
    assert(e == EDGE_STATE_OK);
    val[vlen < sizeof(val) ? vlen : sizeof(val) - 1] = '\0';
    assert(strstr(val, "00:02:5d:d9:21:47") != NULL);
    assert(strstr(val, "open") != NULL);
    assert(strstr(val, "071904926728") != NULL);

    st = edge_e7_callhome_stats(ch);
    assert(st);
    assert(st->accepts >= 1);
    assert(st->sessions_opened >= 1);
    assert(st->rejects_bad_identity == 0);
    assert(st->rejects_not_allowlisted == 0);

    edge_e7_callhome_destroy(ch);
    edge_state_destroy(store);
    printf("  PASS: identity + CLIENT hello → OPEN + inventory session\n");
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

    port = pick_port();
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
    test_reject_unknown_mac();
    printf("All e7_callhome tests passed.\n");
    return 0;
#endif
}
