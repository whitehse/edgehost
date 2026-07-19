/**
 * @file e7_callhome.c
 * @brief Call Home listen + identity + raw NETCONF CLIENT pump (PR-4a).
 */

#define _GNU_SOURCE

#include "edge_e7_callhome.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if EDGEHOST_HAVE_LIBNETCONF
#include "netconf.h"
#endif

#define EDGE_E7_RX_CAP           (64 * 1024)
#define EDGE_E7_TX_CAP           (64 * 1024)
#define EDGE_E7_IDENTITY_TIMEOUT_MS 10000u
#define EDGE_E7_HELLO_TIMEOUT_MS    30000u

/* Per-session host buffers + reduced libnetconf profile (Appendix A). */
#define EDGE_E7_NC_MAX_RPC       (256 * 1024)
#define EDGE_E7_NC_MAX_NOTIF     (64 * 1024)
#define EDGE_E7_NC_MAX_OUTPUT    (256 * 1024)
#define EDGE_E7_NC_EVENT_Q       8

typedef struct {
    edge_e7_sess_state_t state;
    int                  fd;
    int                  slot;
    edge_e7_identity_t   identity;
    char                 peer[EDGE_E7_PEER_ADDR_MAX];
    int                  allowlisted; /* MAC in shelves[] and enabled */
    int                  auto_unknown; /* accepted via auto_subscribe_unknown */

    char                 id_buf[EDGE_E7_IDENTITY_BUF_MAX];
    size_t               id_len;

    uint8_t             *rx; /* host scratch; owned */
    uint8_t             *tx;
    size_t               tx_len;
    size_t               tx_off;

    uint64_t             accepted_ms;
    uint64_t             identity_ms;
    uint64_t             open_ms;

#if EDGEHOST_HAVE_LIBNETCONF
    netconf_ctx_t       *nc;
#endif
} edge_e7_session_t;

struct edge_e7_callhome {
    const edge_config_t *cfg;
    edge_state_store_t  *state;
    edge_ws_hub_t       *hub;

    int                  listen_fd;
    edge_e7_session_t   *sessions;
    uint32_t             max_sessions;
    edge_e7_callhome_stats_t stats;
};

/* ---- utilities ---- */

size_t edge_e7_session_rss_estimate(void)
{
    /* output + max input + event queue + host rx/tx (design Appendix A) */
    size_t event_bytes = (size_t)EDGE_E7_NC_EVENT_Q * 70000u; /* ~netconf_event_t */
    return (size_t)EDGE_E7_NC_MAX_OUTPUT + (size_t)EDGE_E7_NC_MAX_RPC +
           event_bytes + (size_t)EDGE_E7_RX_CAP + (size_t)EDGE_E7_TX_CAP;
}

void edge_e7_netconf_profile(void *cfg_out)
{
#if EDGEHOST_HAVE_LIBNETCONF
    netconf_config_t *c = (netconf_config_t *)cfg_out;
    if (!c) {
        return;
    }
    memset(c, 0, sizeof(*c));
    c->event_queue_size = EDGE_E7_NC_EVENT_Q;
    c->max_rpc_size = EDGE_E7_NC_MAX_RPC;
    c->max_notification_size = EDGE_E7_NC_MAX_NOTIF;
    c->max_output_size = EDGE_E7_NC_MAX_OUTPUT;
#else
    (void)cfg_out;
#endif
}

static uint64_t mono_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void peer_to_str(const struct sockaddr *peer, socklen_t peer_len,
                        char *out, size_t out_sz)
{
    if (!out || out_sz == 0) {
        return;
    }
    out[0] = '\0';
    if (!peer || peer_len < sizeof(sa_family_t)) {
        return;
    }
    if (peer->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)peer;
        inet_ntop(AF_INET, &in->sin_addr, out, (socklen_t)out_sz);
    } else if (peer->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)peer;
        inet_ntop(AF_INET6, &in6->sin6_addr, out, (socklen_t)out_sz);
    }
}

/**
 * Match shelves[] by normalized MAC.
 * @return 1 allowlisted+enabled, 0 present but disabled, -1 not found.
 */
static int shelf_lookup(const edge_config_t *cfg, const char *mac_norm)
{
    uint32_t i;
    char norm[EDGE_E7_MAC_MAX];

    if (!cfg || !mac_norm) {
        return -1;
    }
    for (i = 0; i < cfg->e7_shelf_count; i++) {
        if (cfg->e7_shelves[i].mac[0] == '\0') {
            continue;
        }
        if (edge_e7_mac_normalize(cfg->e7_shelves[i].mac, norm, sizeof(norm)) !=
            0) {
            continue;
        }
        if (strcmp(norm, mac_norm) == 0) {
            return cfg->e7_shelves[i].enabled ? 1 : 0;
        }
    }
    return -1;
}

static void put_session_json(edge_e7_callhome_t *ch, edge_e7_session_t *s,
                             const char *status)
{
    char mac_key[EDGE_E7_MAC_MAX];
    char key[EDGE_STATE_KEY_MAX];
    char json[512];
    int n;
    edge_state_err_t e;

    if (!ch || !ch->state || !s || !s->identity.identity_ok || !status) {
        return;
    }
    if (edge_e7_mac_to_key_seg(s->identity.mac, mac_key, sizeof(mac_key)) != 0) {
        return;
    }
    n = snprintf(key, sizeof(key), "e7/%s/session", mac_key);
    if (n < 0 || (size_t)n >= sizeof(key)) {
        return;
    }
    n = snprintf(json, sizeof(json),
                 "{\"v\":1,\"mac\":\"%s\",\"serial\":\"%s\",\"model\":\"%s\","
                 "\"source_ip\":\"%s\",\"peer\":\"%s\",\"state\":\"%s\","
                 "\"status\":\"%s\"}",
                 s->identity.mac, s->identity.serial, s->identity.model,
                 s->identity.source_ip[0] ? s->identity.source_ip : s->peer,
                 s->peer, status, status);
    if (n < 0 || (size_t)n >= sizeof(json)) {
        return;
    }
    /* PR-4a: put only (coalesced WS notify is PR-4b). hub reserved. */
    (void)ch->hub;
    e = edge_state_put(ch->state, "inventory", key, json, (size_t)n);
    if (e != EDGE_STATE_OK && e != EDGE_STATE_NS_DISABLED) {
        /* best-effort */
    }
}

static void session_clear_nc(edge_e7_session_t *s)
{
#if EDGEHOST_HAVE_LIBNETCONF
    if (s->nc) {
        netconf_destroy(s->nc);
        s->nc = NULL;
    }
#else
    (void)s;
#endif
}

static void session_close(edge_e7_callhome_t *ch, edge_e7_session_t *s)
{
    if (!s || s->state == EDGE_E7_SESS_EMPTY) {
        return;
    }
    if (s->state == EDGE_E7_SESS_OPEN && ch->stats.sessions_open > 0) {
        ch->stats.sessions_open--;
    }
    session_clear_nc(s);
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
    free(s->rx);
    free(s->tx);
    s->rx = NULL;
    s->tx = NULL;
    s->tx_len = s->tx_off = 0;
    s->id_len = 0;
    s->state = EDGE_E7_SESS_EMPTY;
    s->allowlisted = 0;
    s->auto_unknown = 0;
    memset(&s->identity, 0, sizeof(s->identity));
    s->peer[0] = '\0';
}

static edge_e7_session_t *session_alloc(edge_e7_callhome_t *ch)
{
    uint32_t i;
    for (i = 0; i < ch->max_sessions; i++) {
        if (ch->sessions[i].state == EDGE_E7_SESS_EMPTY) {
            edge_e7_session_t *s = &ch->sessions[i];
            memset(s, 0, sizeof(*s));
            s->slot = (int)i;
            s->fd = -1;
            s->state = EDGE_E7_SESS_ACCEPTED;
            s->rx = (uint8_t *)malloc(EDGE_E7_RX_CAP);
            s->tx = (uint8_t *)malloc(EDGE_E7_TX_CAP);
            if (!s->rx || !s->tx) {
                free(s->rx);
                free(s->tx);
                s->rx = s->tx = NULL;
                s->state = EDGE_E7_SESS_EMPTY;
                return NULL;
            }
            return s;
        }
    }
    return NULL;
}

/* ---- identity → CLIENT ---- */

static int find_identity_end(const char *buf, size_t len)
{
    static const char mark[] = "</identity>";
    size_t mlen = sizeof(mark) - 1;
    size_t i;
    if (len < mlen) {
        return -1;
    }
    for (i = 0; i + mlen <= len; i++) {
        if (memcmp(buf + i, mark, mlen) == 0) {
            return (int)(i + mlen);
        }
    }
    return -1;
}

#if EDGEHOST_HAVE_LIBNETCONF
static void drain_nc_events(edge_e7_session_t *s)
{
    netconf_event_t ev;
    while (s->nc && netconf_next_event(s->nc, &ev) == 1) {
        /* PR-4a: no notification apply; just drain to avoid queue overflow. */
        (void)ev;
    }
}

static int session_append_tx(edge_e7_session_t *s, const uint8_t *data, size_t n)
{
    if (!s || !data || n == 0) {
        return 0;
    }
    if (s->tx_off > 0 && s->tx_off < s->tx_len) {
        /* compact */
        memmove(s->tx, s->tx + s->tx_off, s->tx_len - s->tx_off);
        s->tx_len -= s->tx_off;
        s->tx_off = 0;
    } else if (s->tx_off >= s->tx_len) {
        s->tx_len = s->tx_off = 0;
    }
    if (s->tx_len + n > EDGE_E7_TX_CAP) {
        return -1;
    }
    memcpy(s->tx + s->tx_len, data, n);
    s->tx_len += n;
    return 0;
}

static int session_drain_output(edge_e7_session_t *s)
{
    uint8_t tmp[8192];
    size_t n;
    if (!s->nc) {
        return 0;
    }
    for (;;) {
        n = netconf_get_output(s->nc, tmp, sizeof(tmp));
        if (n == 0) {
            break;
        }
        if (session_append_tx(s, tmp, n) != 0) {
            return -1;
        }
    }
    return 0;
}

static int session_start_netconf(edge_e7_callhome_t *ch, edge_e7_session_t *s)
{
    netconf_config_t ncfg;

    (void)ch;
    edge_e7_netconf_profile(&ncfg);
    s->nc = netconf_create_with_config(NETCONF_ROLE_CLIENT, &ncfg);
    if (!s->nc) {
        return -1;
    }
    if (netconf_send_hello(s->nc, NULL, 0) != 0) {
        session_clear_nc(s);
        return -1;
    }
    drain_nc_events(s);
    if (session_drain_output(s) != 0) {
        session_clear_nc(s);
        return -1;
    }
    s->state = EDGE_E7_SESS_HELLO;
    s->identity_ms = mono_now_ms();
    return 0;
}

static void session_check_open(edge_e7_callhome_t *ch, edge_e7_session_t *s)
{
    if (!s->nc) {
        return;
    }
    if (netconf_current_state(s->nc) == NETCONF_STATE_SESSION_OPEN &&
        s->state != EDGE_E7_SESS_OPEN) {
        s->state = EDGE_E7_SESS_OPEN;
        s->open_ms = mono_now_ms();
        ch->stats.sessions_open++;
        ch->stats.sessions_opened++;
        put_session_json(ch, s, "open");
    }
}
#endif /* EDGEHOST_HAVE_LIBNETCONF */

static int session_finish_identity(edge_e7_callhome_t *ch, edge_e7_session_t *s,
                                   size_t id_end)
{
    int look;
    edge_e7_identity_t id;

    if (edge_e7_identity_parse(s->id_buf, id_end, &id) != 0 ||
        !id.identity_ok) {
        ch->stats.rejects_bad_identity++;
        session_close(ch, s);
        return -1;
    }
    s->identity = id;

    look = shelf_lookup(ch->cfg, s->identity.mac);
    if (look == 0) {
        /* present but disabled */
        put_session_json(ch, s, "disabled");
        ch->stats.rejects_disabled++;
        session_close(ch, s);
        return -1;
    }
    if (look < 0) {
        if (!ch->cfg->e7_auto_subscribe_unknown) {
            put_session_json(ch, s, "unconfigured");
            ch->stats.rejects_not_allowlisted++;
            session_close(ch, s);
            return -1;
        }
        s->auto_unknown = 1;
        s->allowlisted = 0;
    } else {
        s->allowlisted = 1;
        s->auto_unknown = 0;
    }

    /* Shift any trailing bytes after identity into a leftover for NETCONF. */
    {
        size_t rem = s->id_len > id_end ? s->id_len - id_end : 0;
        if (rem > 0) {
            memmove(s->id_buf, s->id_buf + id_end, rem);
        }
        s->id_len = rem;
    }

#if EDGEHOST_HAVE_LIBNETCONF
    if (session_start_netconf(ch, s) != 0) {
        ch->stats.rejects_other++;
        put_session_json(ch, s, "error");
        session_close(ch, s);
        return -1;
    }
    /* Feed any leftover post-identity bytes into NETCONF. */
    if (s->id_len > 0 && s->nc) {
        size_t used =
            netconf_feed_input(s->nc, (const uint8_t *)s->id_buf, s->id_len);
        (void)used;
        s->id_len = 0;
        drain_nc_events(s);
        (void)session_drain_output(s);
        session_check_open(ch, s);
    }
    return 0;
#else
    (void)ch;
    session_close(ch, s);
    return -1;
#endif
}

/* ---- I/O pump ---- */

static int session_flush_tx(edge_e7_session_t *s)
{
    while (s->tx_off < s->tx_len) {
        ssize_t n = write(s->fd, s->tx + s->tx_off, s->tx_len - s->tx_off);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return 0;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        s->tx_off += (size_t)n;
    }
    s->tx_len = s->tx_off = 0;
    return 0;
}

static void session_pump_read(edge_e7_callhome_t *ch, edge_e7_session_t *s)
{
    ssize_t n;

    if (s->fd < 0 || !s->rx) {
        return;
    }
    n = read(s->fd, s->rx, EDGE_E7_RX_CAP);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return;
        }
        session_close(ch, s);
        return;
    }
    if (n == 0) {
        session_close(ch, s);
        return;
    }

    if (s->state == EDGE_E7_SESS_ACCEPTED || s->state == EDGE_E7_SESS_IDENTITY) {
        s->state = EDGE_E7_SESS_IDENTITY;
        if (s->id_len + (size_t)n > EDGE_E7_IDENTITY_BUF_MAX) {
            ch->stats.rejects_bad_identity++;
            session_close(ch, s);
            return;
        }
        memcpy(s->id_buf + s->id_len, s->rx, (size_t)n);
        s->id_len += (size_t)n;
        {
            int end = find_identity_end(s->id_buf, s->id_len);
            if (end >= 0) {
                (void)session_finish_identity(ch, s, (size_t)end);
            }
        }
        return;
    }

#if EDGEHOST_HAVE_LIBNETCONF
    if ((s->state == EDGE_E7_SESS_HELLO || s->state == EDGE_E7_SESS_OPEN) &&
        s->nc) {
        size_t used = netconf_feed_input(s->nc, s->rx, (size_t)n);
        (void)used;
        drain_nc_events(s);
        if (session_drain_output(s) != 0) {
            session_close(ch, s);
            return;
        }
        session_check_open(ch, s);
        if (session_flush_tx(s) != 0) {
            session_close(ch, s);
            return;
        }
    }
#else
    (void)ch;
#endif
}

static void session_on_timeout(edge_e7_callhome_t *ch, edge_e7_session_t *s,
                               uint64_t mono_ms)
{
    if (s->state == EDGE_E7_SESS_ACCEPTED || s->state == EDGE_E7_SESS_IDENTITY) {
        if (mono_ms > s->accepted_ms &&
            mono_ms - s->accepted_ms > EDGE_E7_IDENTITY_TIMEOUT_MS) {
            ch->stats.rejects_bad_identity++;
            session_close(ch, s);
        }
    } else if (s->state == EDGE_E7_SESS_HELLO) {
        uint64_t base = s->identity_ms ? s->identity_ms : s->accepted_ms;
        if (mono_ms > base && mono_ms - base > EDGE_E7_HELLO_TIMEOUT_MS) {
            ch->stats.rejects_other++;
            put_session_json(ch, s, "hello_timeout");
            session_close(ch, s);
        }
    }
}

static void session_pump(edge_e7_callhome_t *ch, edge_e7_session_t *s,
                         uint64_t mono_ms)
{
    if (!s || s->state == EDGE_E7_SESS_EMPTY || s->fd < 0) {
        return;
    }
    session_on_timeout(ch, s, mono_ms);
    if (s->state == EDGE_E7_SESS_EMPTY) {
        return;
    }
    if (session_flush_tx(s) != 0) {
        session_close(ch, s);
        return;
    }
    session_pump_read(ch, s);
}

static void try_accept(edge_e7_callhome_t *ch)
{
    struct sockaddr_storage addr;
    socklen_t alen;
    int fd;

    if (!ch || ch->listen_fd < 0) {
        return;
    }
    for (;;) {
        alen = sizeof(addr);
        fd = accept4(ch->listen_fd, (struct sockaddr *)&addr, &alen,
                     SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return;
            }
            return;
        }
        if (edge_e7_callhome_on_accept(ch, fd, (struct sockaddr *)&addr,
                                       alen) != 0) {
            /* on_accept closes fd on failure */
        }
    }
}

/* ---- public API ---- */

edge_e7_callhome_t *edge_e7_callhome_create(const edge_e7_callhome_opts_t *opts)
{
    edge_e7_callhome_t *ch;
    uint32_t max_s;
    size_t est;

#if !EDGEHOST_HAVE_LIBNETCONF
    (void)opts;
    return NULL;
#else
    if (!opts || !opts->cfg || !opts->cfg->e7_enabled) {
        return NULL;
    }
    max_s = opts->cfg->e7_max_sessions;
    if (max_s == 0) {
        max_s = EDGE_E7_MAX_SESSIONS;
    }
    if (max_s > EDGE_E7_MAX_SESSIONS) {
        max_s = EDGE_E7_MAX_SESSIONS;
    }
    est = edge_e7_session_rss_estimate();
    if (opts->cfg->e7_rss_budget_bytes > 0 &&
        (size_t)max_s * est > opts->cfg->e7_rss_budget_bytes) {
        fprintf(stderr,
                "edgehost: e7_callhome RSS estimate %zu * %u > budget %zu\n",
                est, max_s, opts->cfg->e7_rss_budget_bytes);
        return NULL;
    }

    ch = (edge_e7_callhome_t *)calloc(1, sizeof(*ch));
    if (!ch) {
        return NULL;
    }
    ch->cfg = opts->cfg;
    ch->state = opts->state;
    ch->hub = opts->hub;
    ch->listen_fd = -1;
    ch->max_sessions = max_s;
    ch->sessions =
        (edge_e7_session_t *)calloc(max_s, sizeof(edge_e7_session_t));
    if (!ch->sessions) {
        free(ch);
        return NULL;
    }
    {
        uint32_t i;
        for (i = 0; i < max_s; i++) {
            ch->sessions[i].fd = -1;
            ch->sessions[i].slot = (int)i;
            ch->sessions[i].state = EDGE_E7_SESS_EMPTY;
        }
    }

    /* Enable producer namespaces when store is available. */
    if (ch->state) {
        (void)edge_state_ns_set_enabled(ch->state, "inventory", 1);
        (void)edge_state_ns_set_enabled(ch->state, "net.pon", 1);
    }
    return ch;
#endif
}

void edge_e7_callhome_destroy(edge_e7_callhome_t *ch)
{
    if (!ch) {
        return;
    }
    edge_e7_callhome_close_all(ch);
    free(ch->sessions);
    free(ch);
}

int edge_e7_callhome_enabled(const edge_e7_callhome_t *ch)
{
    return ch && ch->cfg && ch->cfg->e7_enabled;
}

int edge_e7_callhome_bind(edge_e7_callhome_t *ch)
{
    int fd;
    int on = 1;
    struct sockaddr_in addr;
    const char *host;
    uint16_t port;

    if (!ch || !ch->cfg) {
        return -1;
    }
    if (ch->listen_fd >= 0) {
        return 0;
    }

    host = ch->cfg->e7_listen_host[0] ? ch->cfg->e7_listen_host : "127.0.0.1";
    port = ch->cfg->e7_listen_port ? ch->cfg->e7_listen_port : 4334u;

    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("edgehost e7: socket");
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0) {
        perror("edgehost e7: REUSEADDR");
        close(fd);
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (strcmp(host, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "edgehost e7: invalid listen host %s\n", host);
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("edgehost e7: bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 128) != 0) {
        perror("edgehost e7: listen");
        close(fd);
        return -1;
    }
    if (set_nonblock(fd) != 0) {
        perror("edgehost e7: nonblock");
        close(fd);
        return -1;
    }
    ch->listen_fd = fd;
    fprintf(stderr, "edgehost: e7_callhome listening on %s:%u (raw)\n", host,
            (unsigned)port);
    return 0;
}

int edge_e7_callhome_listen_fd(const edge_e7_callhome_t *ch)
{
    return ch ? ch->listen_fd : -1;
}

int edge_e7_callhome_on_accept(edge_e7_callhome_t *ch, int fd,
                               const struct sockaddr *peer, socklen_t peer_len)
{
    edge_e7_session_t *s;

    if (!ch || fd < 0) {
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    if (set_nonblock(fd) != 0) {
        close(fd);
        ch->stats.rejects_other++;
        return -1;
    }
    s = session_alloc(ch);
    if (!s) {
        close(fd);
        ch->stats.rejects_capacity++;
        return -1;
    }
    s->fd = fd;
    s->state = EDGE_E7_SESS_ACCEPTED;
    s->accepted_ms = mono_now_ms();
    peer_to_str(peer, peer_len, s->peer, sizeof(s->peer));
    ch->stats.accepts++;
    return 0;
}

void edge_e7_callhome_poll(edge_e7_callhome_t *ch, uint64_t mono_ms)
{
    uint32_t i;

    if (!ch) {
        return;
    }
    if (mono_ms == 0) {
        mono_ms = mono_now_ms();
    }
    try_accept(ch);
    for (i = 0; i < ch->max_sessions; i++) {
        if (ch->sessions[i].state != EDGE_E7_SESS_EMPTY) {
            session_pump(ch, &ch->sessions[i], mono_ms);
        }
    }
}

void edge_e7_callhome_on_tick(edge_e7_callhome_t *ch, uint64_t mono_ms)
{
    edge_e7_callhome_poll(ch, mono_ms);
}

void edge_e7_callhome_close_all(edge_e7_callhome_t *ch)
{
    uint32_t i;
    if (!ch) {
        return;
    }
    for (i = 0; i < ch->max_sessions; i++) {
        session_close(ch, &ch->sessions[i]);
    }
    if (ch->listen_fd >= 0) {
        close(ch->listen_fd);
        ch->listen_fd = -1;
    }
}

const edge_e7_callhome_stats_t *edge_e7_callhome_stats(
    const edge_e7_callhome_t *ch)
{
    return ch ? &ch->stats : NULL;
}

edge_e7_sess_state_t edge_e7_callhome_session_state_by_mac(
    const edge_e7_callhome_t *ch, const char *mac)
{
    char norm[EDGE_E7_MAC_MAX];
    uint32_t i;

    if (!ch || !mac ||
        edge_e7_mac_normalize(mac, norm, sizeof(norm)) != 0) {
        return EDGE_E7_SESS_EMPTY;
    }
    for (i = 0; i < ch->max_sessions; i++) {
        const edge_e7_session_t *s = &ch->sessions[i];
        if (s->state == EDGE_E7_SESS_EMPTY) {
            continue;
        }
        if (s->identity.identity_ok && strcmp(s->identity.mac, norm) == 0) {
            return s->state;
        }
    }
    return EDGE_E7_SESS_EMPTY;
}

uint32_t edge_e7_callhome_open_count(const edge_e7_callhome_t *ch)
{
    return ch ? (uint32_t)ch->stats.sessions_open : 0;
}
