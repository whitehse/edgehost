/**
 * @file e7_callhome.c
 * @brief Call Home listen + identity + subscribe + lab.v1 apply + K16 + REST.
 */

#define _GNU_SOURCE

#include "edge_e7_callhome.h"
#include "edge_state_notify.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#if EDGEHOST_HAVE_LIBNETCONF
#include "netconf.h"
#endif
#if EDGEHOST_E7_CHSSH_AVAILABLE
#include "chssh.h"
#endif
#if EDGEHOST_HAVE_LIBYANG
#include "yang.h"
#endif

#define EDGE_E7_RX_CAP           (64 * 1024)
#define EDGE_E7_TX_CAP           (64 * 1024)
#define EDGE_E7_IDENTITY_TIMEOUT_MS 10000u
#define EDGE_E7_HELLO_TIMEOUT_MS    30000u
/**
 * SSH ClientAlive keepalive once SESSION_OPEN (libchssh path).
 * OpenSSH on the E7 tracks *client→server* silence (ClientAliveInterval),
 * not RX of notifications. Field dials close ~90s without our TX; send
 * every 15s wall-clock from last keepalive TX (not last RX).
 */
#define EDGE_E7_KEEPALIVE_MS        15000u
/** After identity (auto): wait this long for peer bytes before speaking. */
#define EDGE_E7_POST_ID_WAIT_MS     400u
/** Silent listen after identity before we speak (raw probe). */
#define EDGE_E7_PROBE_SILENT_MS     1500u
/** Wait after proprietary identity ACK before escalating. */
#define EDGE_E7_PROBE_ACK_WAIT_MS   800u
/**
 * Field SSH path: after identity, stay silent this long and capture any
 * peer-first bytes before identity ACK ladder / SSH server banner.
 * Prior field dials: E7 waits if we stay silent; any wrong TX → immediate RST.
 */
#define EDGE_E7_SSH_HOLD_MS         3000u
/** Wait after each identity-layer ACK attempt on the SSH field ladder. */
#define EDGE_E7_SSH_ACK_WAIT_MS     800u

/* Per-MAC post-identity probe modes (raw). Advanced on pre-OPEN peer_eof. */
enum {
    EDGE_E7_PROBE_SILENT = 0,      /* listen only — capture peer-first bytes */
    EDGE_E7_PROBE_VERSION_ACK = 1, /* proprietary identity ACK (no NETCONF) */
    EDGE_E7_PROBE_VERSION_ACK2 = 2,/* alternate ACK forms */
    EDGE_E7_PROBE_NC_CLIENT = 3,   /* NETCONF client hello base:1.0 */
    EDGE_E7_PROBE_NC_SERVER = 4,   /* NETCONF server hello + session-id */
    EDGE_E7_PROBE_COUNT = 5
};

/*
 * Same-dial post-identity phases for transport:ssh field diagnostics.
 * 0 = silent hold, 1..N-1 = identity ACK variants, N = SSH server banner.
 */
enum {
    EDGE_E7_SSH_PHASE_HOLD = 0,
    EDGE_E7_SSH_PHASE_ACK0 = 1,
    EDGE_E7_SSH_PHASE_ACK1 = 2,
    EDGE_E7_SSH_PHASE_ACK2 = 3,
    EDGE_E7_SSH_PHASE_ACK3 = 4,
    EDGE_E7_SSH_PHASE_ACK4 = 5,
    EDGE_E7_SSH_PHASE_SSH  = 6
};

/* Per-session host buffers + reduced libnetconf profile (Appendix A). */
#define EDGE_E7_NC_MAX_RPC       (256 * 1024)
#define EDGE_E7_NC_MAX_NOTIF     (64 * 1024)
#define EDGE_E7_NC_MAX_OUTPUT    (256 * 1024)
#define EDGE_E7_NC_EVENT_Q       8

#define EDGE_E7_DIRTY_NS_MAX     32

typedef struct {
    uint64_t id;
    uint64_t t_ms;
    char     stage[EDGE_E7_TRACE_STAGE_MAX];
    char     mac[EDGE_E7_MAC_MAX];
    char     peer[EDGE_E7_PEER_ADDR_MAX];
    char     detail[EDGE_E7_TRACE_DETAIL_MAX];
} edge_e7_trace_ev_t;

typedef struct {
    edge_e7_sess_state_t state;
    int                  fd;           /* Call Home TCP (identity + SSH/NETCONF) */
    int                  slot;
    edge_e7_identity_t   identity;
    char                 peer[EDGE_E7_PEER_ADDR_MAX];
    int                  allowlisted; /* MAC in shelves[] and enabled */
    int                  auto_unknown; /* accepted via auto_subscribe_unknown */
    int                  last_nc_state; /* last traced netconf_state_t or -1 */
    int                  sub_sent;     /* create_subscription issued */
    int                  sub_ok;       /* subscription rpc-ok */
    int                  sub_msg_id;
    int                  hello_sent;   /* netconf_send_hello issued */
    int                  use_ssh;      /* transport is SSH Call Home (same fd) */
    int                  use_chssh;     /* 1 = libchssh path (preferred) */
    int                  chssh_ready;   /* subsystem netconf open */
    int                  nc_as_server; /* raw-probe only: NETCONF server hello */
    int                  ssh_banner_flushed;
    int                  saw_ssh_rx;
    uint8_t              probe_mode;   /* EDGE_E7_PROBE_* (raw transport only) */
    uint8_t              probe_spoke;
    uint8_t              probe_ack_sent;
    uint8_t              ssh_phase;    /* EDGE_E7_SSH_PHASE_* (transport:ssh) */
    uint8_t              ssh_resume;   /* phase after hold (from runtime) */
    uint8_t              first_tx_logged;

    /* Per-dial capture summary (host stream log) */
    uint64_t             cap_t0_ms;       /* first app byte (usually identity RX) */
    uint64_t             cap_ident_ms;    /* identity complete time */
    uint64_t             cap_first_tx_ms; /* first NMS app TX after identity */
    uint32_t             cap_rx_bytes;
    uint32_t             cap_tx_bytes;
    uint16_t             cap_rx_chunks;
    uint16_t             cap_tx_chunks;
    uint8_t              cap_saw_ident;
    uint8_t              cap_saw_ssh_tx;
    uint8_t              cap_saw_ack_tx;
    uint8_t              cap_saw_ssh_rx;
    uint8_t              cap_summary_done;
    char                 cap_first_tx_tag[40];

    char                 id_buf[EDGE_E7_IDENTITY_BUF_MAX];
    size_t               id_len;

    uint8_t             *rx; /* host scratch; owned */
    uint8_t             *tx;
    size_t               tx_len;
    size_t               tx_off;

    uint64_t             accepted_ms;
    uint64_t             identity_ms;
    uint64_t             open_ms;
    uint64_t             last_keepalive_ms; /* last SSH keepalive TX (open) */
    uint64_t             last_activity_ms;  /* last app RX/TX while open */

#if EDGEHOST_HAVE_LIBNETCONF
    netconf_ctx_t       *nc;
#endif
#if EDGEHOST_E7_CHSSH_AVAILABLE
    chssh_ctx_t         *chssh;
#endif
} edge_e7_session_t;

typedef struct {
    int  used;
    char ns[EDGE_E7_DIRTY_NS_MAX];
    char key[EDGE_STATE_KEY_MAX];
} edge_e7_dirty_slot_t;

typedef struct {
    int      used;
    int      complete;
    uint32_t message_id;
    int      shelf_slot;
    char     mac[EDGE_E7_MAC_MAX];
    char     cmd_id[EDGE_E7_CMD_ID_MAX];
    uint64_t deadline_ms;
} edge_e7_cmd_slot_t;

struct edge_e7_callhome {
    const edge_config_t *cfg;
    edge_state_store_t  *state;
    edge_ws_hub_t       *hub;

    int                  listen_fd;
    /* Bound listen coords (for SIGHUP change detection; no live rebind). */
    char                 bound_host[EDGE_CONFIG_HOST_MAX];
    uint16_t             bound_port;
    int                  bound_enabled;
    edge_e7_session_t   *sessions;
    uint32_t             max_sessions;
    edge_e7_callhome_stats_t stats;

    /* K16 dirty-set (host-owned; fixed cap) */
    edge_e7_dirty_slot_t *dirty;
    uint32_t              dirty_cap;
    uint32_t              dirty_used;
    uint64_t              last_flush_ms;

    /* Runtime allowlist (YAML seed + REST; optional e7_allowlist_path file) */
    edge_e7_runtime_shelf_t runtime[EDGE_E7_RUNTIME_SHELVES_MAX];
    uint32_t                runtime_count;

    /* Command correlation (message_id → cmd_id) */
    edge_e7_cmd_slot_t cmds[EDGE_E7_CMD_TABLE_MAX];
    uint32_t           cmd_seq;

    /* Connection progress ring for SPA /api/v1/e7/events */
    edge_e7_trace_ev_t trace[EDGE_E7_TRACE_CAP];
    uint32_t           trace_head; /* next write slot */
    uint32_t           trace_count;
    uint64_t           trace_seq; /* monotonic event id */

    /*
     * Host-visible Call Home stream capture (what e7_callhome read/write sees
     * after io_uring accept). Not AF_PACKET; records app-stream as PCAP RAW IP.
     */
    FILE    *pcap_fp;
    FILE    *pcap_text_fp;
    char     pcap_path[512];
    char     pcap_text_path[512];
    uint32_t pcap_seq_c2s; /* synthetic TCP seq NMS→E7 */
#if EDGEHOST_HAVE_LIBYANG
    yang_ctx_t *yang;           /* shared schema registry for all sessions */
    char        yang_cache_dir[512]; /* e.g. var/yang */
#endif
    uint32_t pcap_seq_s2c; /* E7→NMS */
    uint64_t pcap_pkts;
    uint64_t pcap_bytes;
    uint64_t pcap_dials; /* completed dial summaries written */
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
 * Match runtime allowlist by normalized MAC.
 * @return 1 allowlisted+enabled, 0 present but disabled, -1 not found.
 */
static int shelf_lookup_runtime(const edge_e7_callhome_t *ch,
                                const char *mac_norm)
{
    uint32_t i;

    if (!ch || !mac_norm) {
        return -1;
    }
    for (i = 0; i < EDGE_E7_RUNTIME_SHELVES_MAX; i++) {
        if (!ch->runtime[i].used) {
            continue;
        }
        if (strcmp(ch->runtime[i].mac, mac_norm) == 0) {
            return ch->runtime[i].enabled ? 1 : 0;
        }
    }
    return -1;
}

static edge_e7_runtime_shelf_t *runtime_find(edge_e7_callhome_t *ch,
                                             const char *mac_norm)
{
    uint32_t i;
    if (!ch || !mac_norm) {
        return NULL;
    }
    for (i = 0; i < EDGE_E7_RUNTIME_SHELVES_MAX; i++) {
        if (ch->runtime[i].used && strcmp(ch->runtime[i].mac, mac_norm) == 0) {
            return &ch->runtime[i];
        }
    }
    return NULL;
}

static edge_e7_runtime_shelf_t *runtime_alloc(edge_e7_callhome_t *ch)
{
    uint32_t i;
    if (!ch) {
        return NULL;
    }
    for (i = 0; i < EDGE_E7_RUNTIME_SHELVES_MAX; i++) {
        if (!ch->runtime[i].used) {
            memset(&ch->runtime[i], 0, sizeof(ch->runtime[i]));
            ch->runtime[i].used = 1;
            ch->runtime_count++;
            return &ch->runtime[i];
        }
    }
    return NULL;
}

static void put_config_json(edge_e7_callhome_t *ch,
                            const edge_e7_runtime_shelf_t *rs)
{
    char mac_key[EDGE_E7_MAC_MAX];
    char key[EDGE_STATE_KEY_MAX];
    char json[512];
    int n;
    edge_state_err_t e;

    if (!ch || !ch->state || !rs || !rs->used) {
        return;
    }
    if (edge_e7_mac_to_key_seg(rs->mac, mac_key, sizeof(mac_key)) != 0) {
        return;
    }
    n = snprintf(key, sizeof(key), "e7/%s/config", mac_key);
    if (n < 0 || (size_t)n >= sizeof(key)) {
        return;
    }
    n = snprintf(
        json, sizeof(json),
        "{\"v\":1,\"mac\":\"%s\",\"label\":\"%s\",\"enabled\":%s,"
        "\"note\":\"not written to YAML; set plugins.e7_callhome.allowlist_path "
        "for file durability\"}",
        rs->mac, rs->label[0] ? rs->label : "", rs->enabled ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof(json)) {
        return;
    }
    e = edge_state_put_and_notify(ch->state, ch->hub, "inventory", key, json,
                                  (size_t)n, NULL, 0);
    if (e == EDGE_STATE_OK) {
        ch->stats.state_puts++;
        ch->stats.ws_fanouts++;
    }
}

/**
 * Upsert YAML shelves into runtime (YAML MAC wins for listed entries).
 * Does not remove runtime-only shelves — used for create seed and merge.
 */
static void seed_runtime_from_yaml(edge_e7_callhome_t *ch)
{
    uint32_t i;

    if (!ch || !ch->cfg) {
        return;
    }
    for (i = 0; i < ch->cfg->e7_shelf_count; i++) {
        char norm[EDGE_E7_MAC_MAX];
        edge_e7_runtime_shelf_t *rs;
        int created = 0;

        if (ch->cfg->e7_shelves[i].mac[0] == '\0') {
            continue;
        }
        if (edge_e7_mac_normalize(ch->cfg->e7_shelves[i].mac, norm,
                                  sizeof(norm)) != 0) {
            continue;
        }
        rs = runtime_find(ch, norm);
        if (!rs) {
            rs = runtime_alloc(ch);
            if (!rs) {
                break;
            }
            created = 1;
            memcpy(rs->mac, norm, sizeof(rs->mac));
        }
        if (ch->cfg->e7_shelves[i].shelf_id[0]) {
            snprintf(rs->label, sizeof(rs->label), "%s",
                     ch->cfg->e7_shelves[i].shelf_id);
        } else if (created) {
            rs->label[0] = '\0';
        } else {
            /* merge: YAML empty label clears? keep prior unless replace path */
            rs->label[0] = '\0';
        }
        rs->enabled = ch->cfg->e7_shelves[i].enabled ? 1 : 0;
        rs->from_yaml = 1;
        if (created) {
            /*
             * YAML seed: NETCONF client hello (unit tests + design K13).
             * Field shelves that fail will probe_advance to silent → version_ack.
             */
            rs->probe_mode = EDGE_E7_PROBE_NC_CLIENT;
        }
        put_config_json(ch, rs);
    }
}

/**
 * Load durable allowlist file (PR-10 interim). Format (one shelf per line):
 *   mac=<mac> enabled=<0|1> label=<optional text to EOL>
 * Lines starting with # and blank lines ignored. Merges into runtime
 * (file wins for listed MACs; does not clear runtime-only entries).
 */
static void load_runtime_from_file(edge_e7_callhome_t *ch)
{
    FILE *fp;
    char line[512];

    if (!ch || !ch->cfg || ch->cfg->e7_allowlist_path[0] == '\0') {
        return;
    }
    fp = fopen(ch->cfg->e7_allowlist_path, "r");
    if (!fp) {
        return; /* missing file is OK on first run */
    }
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        char mac_raw[EDGE_CONFIG_E7_MAC_MAX];
        char norm[EDGE_E7_MAC_MAX];
        char label[EDGE_CONFIG_E7_SHELF_ID_MAX];
        int enabled = 1;
        edge_e7_runtime_shelf_t *rs;
        char *tok;

        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0' || *p == '#' || *p == '\n' || *p == '\r') {
            continue;
        }
        mac_raw[0] = '\0';
        label[0] = '\0';
        /* tokenize space-separated key=value; label may contain spaces after = */
        while (*p) {
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p == '\0' || *p == '\n' || *p == '\r') {
                break;
            }
            tok = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
                p++;
            }
            if (strncmp(tok, "label=", 6) == 0) {
                /* rest of line after label= */
                char *src = tok + 6;
                size_t n = 0;
                while (src[n] && src[n] != '\n' && src[n] != '\r' &&
                       n + 1 < sizeof(label)) {
                    label[n] = src[n];
                    n++;
                }
                label[n] = '\0';
                break;
            }
            if (*p) {
                *p++ = '\0';
            }
            if (strncmp(tok, "mac=", 4) == 0) {
                snprintf(mac_raw, sizeof(mac_raw), "%s", tok + 4);
            } else if (strncmp(tok, "enabled=", 8) == 0) {
                enabled = (tok[8] == '0') ? 0 : 1;
            }
        }
        if (mac_raw[0] == '\0' ||
            edge_e7_mac_normalize(mac_raw, norm, sizeof(norm)) != 0) {
            continue;
        }
        rs = runtime_find(ch, norm);
        if (!rs) {
            rs = runtime_alloc(ch);
            if (!rs) {
                break;
            }
            memcpy(rs->mac, norm, sizeof(rs->mac));
            rs->from_yaml = 0;
            /* Field allowlist: start with silent listen probe. */
            rs->probe_mode = EDGE_E7_PROBE_SILENT;
        }
        rs->enabled = enabled ? 1 : 0;
        if (label[0]) {
            snprintf(rs->label, sizeof(rs->label), "%s", label);
        }
        put_config_json(ch, rs);
    }
    fclose(fp);
}

/** Rewrite durable allowlist file from current runtime table. Best-effort. */
static void save_runtime_to_file(edge_e7_callhome_t *ch)
{
    FILE *fp;
    uint32_t i;
    char tmp[EDGE_CONFIG_PATH_MAX + 8];
    int n;

    if (!ch || !ch->cfg || ch->cfg->e7_allowlist_path[0] == '\0') {
        return;
    }
    n = snprintf(tmp, sizeof(tmp), "%s.tmp", ch->cfg->e7_allowlist_path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        return;
    }
    fp = fopen(tmp, "w");
    if (!fp) {
        return;
    }
    fprintf(fp, "# edgehost e7 allowlist v1 (auto-written; PR-10 interim)\n");
    for (i = 0; i < EDGE_E7_RUNTIME_SHELVES_MAX; i++) {
        const edge_e7_runtime_shelf_t *rs = &ch->runtime[i];
        if (!rs->used || rs->mac[0] == '\0') {
            continue;
        }
        fprintf(fp, "mac=%s enabled=%d", rs->mac, rs->enabled ? 1 : 0);
        if (rs->label[0]) {
            fprintf(fp, " label=%s", rs->label);
        }
        fputc('\n', fp);
    }
    if (fclose(fp) != 0) {
        (void)unlink(tmp);
        return;
    }
    if (rename(tmp, ch->cfg->e7_allowlist_path) != 0) {
        (void)unlink(tmp);
    }
}

static void runtime_clear_all(edge_e7_callhome_t *ch)
{
    if (!ch) {
        return;
    }
    memset(ch->runtime, 0, sizeof(ch->runtime));
    ch->runtime_count = 0;
}

static int e7_transport_is_ssh(const char *t)
{
    /* Case-insensitive "ssh" without strcasecmp (portable). */
    char a, b, c;
    if (!t || !t[0] || !t[1] || !t[2] || t[3] != '\0') {
        return 0;
    }
    a = t[0];
    b = t[1];
    c = t[2];
    if (a >= 'A' && a <= 'Z') {
        a = (char)(a - 'A' + 'a');
    }
    if (b >= 'A' && b <= 'Z') {
        b = (char)(b - 'A' + 'a');
    }
    if (c >= 'A' && c <= 'Z') {
        c = (char)(c - 'A' + 'a');
    }
    return a == 's' && b == 's' && c == 'h';
}

static int e7_transport_is_auto(const char *t)
{
    char a, b, c, d;
    if (!t || !t[0] || !t[1] || !t[2] || !t[3] || t[4] != '\0') {
        return 0;
    }
    a = t[0];
    b = t[1];
    c = t[2];
    d = t[3];
    if (a >= 'A' && a <= 'Z') {
        a = (char)(a - 'A' + 'a');
    }
    if (b >= 'A' && b <= 'Z') {
        b = (char)(b - 'A' + 'a');
    }
    if (c >= 'A' && c <= 'Z') {
        c = (char)(c - 'A' + 'a');
    }
    if (d >= 'A' && d <= 'Z') {
        d = (char)(d - 'A' + 'a');
    }
    return a == 'a' && b == 'u' && c == 't' && d == 'o';
}

/** True when config may use SSH (ssh or auto). */
static int e7_transport_may_ssh(const char *t)
{
    return e7_transport_is_ssh(t) || e7_transport_is_auto(t);
}

static edge_e7_session_t *session_find_by_mac(edge_e7_callhome_t *ch,
                                              const char *mac_norm)
{
    uint32_t i;
    if (!ch || !mac_norm) {
        return NULL;
    }
    for (i = 0; i < ch->max_sessions; i++) {
        edge_e7_session_t *s = &ch->sessions[i];
        if (s->state == EDGE_E7_SESS_EMPTY) {
            continue;
        }
        if (s->identity.identity_ok && strcmp(s->identity.mac, mac_norm) == 0) {
            return s;
        }
    }
    return NULL;
}

const char *edge_e7_sess_state_name(edge_e7_sess_state_t st)
{
    switch (st) {
    case EDGE_E7_SESS_EMPTY:     return "empty";
    case EDGE_E7_SESS_ACCEPTED:  return "accepted";
    case EDGE_E7_SESS_IDENTITY:  return "identity";
    case EDGE_E7_SESS_POST_ID:   return "post_identity";
    case EDGE_E7_SESS_SSH:       return "ssh";
    case EDGE_E7_SESS_HELLO:     return "hello";
    case EDGE_E7_SESS_OPEN:      return "open";
    case EDGE_E7_SESS_ERROR:     return "error";
    case EDGE_E7_SESS_CLOSING:   return "closing";
    default:                     return "unknown";
    }
}

#if EDGEHOST_HAVE_LIBNETCONF
static const char *e7_nc_state_name(netconf_state_t st)
{
    switch (st) {
    case NETCONF_STATE_IDLE:                 return "idle";
    case NETCONF_STATE_SSH_CONNECTING:       return "ssh_connecting";
    case NETCONF_STATE_SSH_AUTHENTICATING:   return "ssh_authenticating";
    case NETCONF_STATE_CAPABILITY_EXCHANGE:  return "capability_exchange";
    case NETCONF_STATE_SESSION_OPEN:         return "session_open";
    case NETCONF_STATE_CLOSING:              return "closing";
    case NETCONF_STATE_CLOSED:               return "closed";
    case NETCONF_STATE_ERROR:                return "error";
    default:                                 return "unknown";
    }
}
#endif

/** Append one connection-progress event (ring; oldest dropped). */
static void e7_trace(edge_e7_callhome_t *ch, const char *stage, const char *mac,
                     const char *peer, const char *detail_fmt, ...)
{
    edge_e7_trace_ev_t *ev;
    va_list ap;
    int n;

    if (!ch || !stage || !stage[0]) {
        return;
    }
    ev = &ch->trace[ch->trace_head];
    memset(ev, 0, sizeof(*ev));
    ch->trace_seq++;
    ev->id = ch->trace_seq;
    ev->t_ms = mono_now_ms();
    snprintf(ev->stage, sizeof(ev->stage), "%s", stage);
    if (mac && mac[0]) {
        snprintf(ev->mac, sizeof(ev->mac), "%s", mac);
    }
    if (peer && peer[0]) {
        snprintf(ev->peer, sizeof(ev->peer), "%s", peer);
    }
    if (detail_fmt && detail_fmt[0]) {
        va_start(ap, detail_fmt);
        n = vsnprintf(ev->detail, sizeof(ev->detail), detail_fmt, ap);
        va_end(ap);
        if (n < 0) {
            ev->detail[0] = '\0';
        }
    }
    ch->trace_head = (ch->trace_head + 1u) % EDGE_E7_TRACE_CAP;
    if (ch->trace_count < EDGE_E7_TRACE_CAP) {
        ch->trace_count++;
    }
}

/**
 * Log raw bytes as ascii peek + chunked hex (fits EDGE_E7_TRACE_DETAIL_MAX).
 * Used for field identity / first TX / post-identity RX diagnostics.
 */
static void e7_trace_bytes(edge_e7_callhome_t *ch, const char *stage,
                           const char *mac, const char *peer,
                           const char *label, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    char ascii[56];
    size_t i, alen, off;
    const size_t chunk = 28; /* 56 hex chars + prefixes fit in detail */

    if (!ch || !stage) {
        return;
    }
    if (!p || len == 0) {
        e7_trace(ch, stage, mac ? mac : "", peer ? peer : "",
                 "%s 0B (empty)", label ? label : "bytes");
        return;
    }
    alen = len < sizeof(ascii) - 1 ? len : sizeof(ascii) - 1;
    for (i = 0; i < alen; i++) {
        unsigned char c = p[i];
        ascii[i] = (c >= 32 && c < 127) ? (char)c : '.';
    }
    ascii[alen] = '\0';
    e7_trace(ch, stage, mac ? mac : "", peer ? peer : "",
             "%s %zuB ascii=\"%s\"%s", label ? label : "bytes", len, ascii,
             len > alen ? "…" : "");

    for (off = 0; off < len; off += chunk) {
        char hex[(28 * 2) + 1];
        size_t n = len - off;
        size_t j;
        if (n > chunk) {
            n = chunk;
        }
        for (j = 0; j < n; j++) {
            static const char xd[] = "0123456789abcdef";
            hex[j * 2] = xd[p[off + j] >> 4];
            hex[j * 2 + 1] = xd[p[off + j] & 0x0f];
        }
        hex[n * 2] = '\0';
        e7_trace(ch, stage, mac ? mac : "", peer ? peer : "",
                 "%s hex[%zu..%zu]=%s", label ? label : "bytes", off,
                 off + n - 1, hex);
    }
}

/** JSON-escape into @p out including surrounding quotes. @return length or -1. */
static int json_escape_str(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    if (!out || out_sz < 3) {
        return -1;
    }
    out[o++] = '"';
    if (in) {
        for (; *in; in++) {
            unsigned char c = (unsigned char)*in;
            if (c == '"' || c == '\\') {
                if (o + 3 >= out_sz) {
                    return -1;
                }
                out[o++] = '\\';
                out[o++] = (char)c;
            } else if (c < 0x20u || c == 0x7fu) {
                /* drop control chars */
                continue;
            } else {
                if (o + 2 >= out_sz) {
                    return -1;
                }
                out[o++] = (char)c;
            }
        }
    }
    if (o + 2 > out_sz) {
        return -1;
    }
    out[o++] = '"';
    out[o] = '\0';
    return (int)o;
}

/* ---- K16 dirty-set ---- */

static uint32_t dirty_hash(const char *ns, const char *key)
{
    uint32_t h = 5381u;
    const unsigned char *p;
    for (p = (const unsigned char *)ns; *p; p++) {
        h = ((h << 5) + h) + *p;
    }
    h = ((h << 5) + h) + (uint32_t)':';
    for (p = (const unsigned char *)key; *p; p++) {
        h = ((h << 5) + h) + *p;
    }
    return h;
}

static void dirty_broadcast_key(edge_e7_callhome_t *ch, const char *ns,
                                const char *key)
{
    char val[EDGE_STATE_VALUE_DEFAULT];
    size_t vlen = 0;
    char rid[EDGE_WS_REQUEST_ID];
    edge_state_err_t e;

    if (!ch || !ch->hub || !ch->state || !ns || !key) {
        return;
    }
    e = edge_state_get(ch->state, ns, key, val, sizeof(val), &vlen);
    if (e != EDGE_STATE_OK) {
        return;
    }
    edge_ws_hub_mint_request_id(ch->hub, NULL, rid, sizeof(rid));
    if (edge_ws_hub_broadcast_state_changed(ch->hub, ns, key, "put", val, vlen,
                                            rid) > 0) {
        ch->stats.ws_fanouts++;
    }
}

/**
 * Mark (ns,key) dirty. @return 0 ok, -1 table full (overflow — caller must
 * force-notify).
 */
static int dirty_mark(edge_e7_callhome_t *ch, const char *ns, const char *key)
{
    uint32_t i, start, cap;

    if (!ch || !ch->dirty || ch->dirty_cap == 0 || !ns || !key || !key[0]) {
        return -1;
    }
    if (strlen(ns) >= EDGE_E7_DIRTY_NS_MAX ||
        strlen(key) >= EDGE_STATE_KEY_MAX) {
        return -1;
    }
    cap = ch->dirty_cap;
    start = dirty_hash(ns, key) % cap;
    for (i = 0; i < cap; i++) {
        uint32_t idx = (start + i) % cap;
        edge_e7_dirty_slot_t *s = &ch->dirty[idx];
        if (s->used && strcmp(s->ns, ns) == 0 && strcmp(s->key, key) == 0) {
            return 0; /* already dirty */
        }
        if (!s->used) {
            s->used = 1;
            snprintf(s->ns, sizeof(s->ns), "%s", ns);
            snprintf(s->key, sizeof(s->key), "%s", key);
            ch->dirty_used++;
            return 0;
        }
    }
    return -1; /* full */
}

static void dirty_flush(edge_e7_callhome_t *ch)
{
    uint32_t i;
    int any = 0;

    if (!ch || !ch->dirty || ch->dirty_used == 0) {
        return;
    }
    for (i = 0; i < ch->dirty_cap; i++) {
        edge_e7_dirty_slot_t *s = &ch->dirty[i];
        if (!s->used) {
            continue;
        }
        dirty_broadcast_key(ch, s->ns, s->key);
        s->used = 0;
        s->ns[0] = '\0';
        s->key[0] = '\0';
        any = 1;
    }
    ch->dirty_used = 0;
    if (any) {
        ch->stats.coalesce_flush++;
    }
}

/**
 * After state put for a high-rate key: mark dirty, or force-notify on overflow.
 */
static void notify_coalesce(edge_e7_callhome_t *ch, const char *ns,
                            const char *key)
{
    if (!ch || !ns || !key) {
        return;
    }
    if (!ch->hub) {
        return;
    }
    if (dirty_mark(ch, ns, key) != 0) {
        ch->stats.coalesce_overflow++;
        dirty_broadcast_key(ch, ns, key);
    }
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
    /* Low-rate path: immediate STATE_CHANGED (coalesce=0). */
    e = edge_state_put_and_notify(ch->state, ch->hub, "inventory", key, json,
                                  (size_t)n, NULL, 0);
    if (e == EDGE_STATE_OK) {
        ch->stats.state_puts++;
    }
}

/* Minimal lab.v1 tag extract for dirty-key construction (mirrors apply). */
static int xml_tag_text(const char *xml, size_t len, const char *tag, char *out,
                        size_t out_sz)
{
    char open[64];
    size_t tlen, olen;
    size_t i;

    if (!xml || !tag || !out || out_sz == 0) {
        return -1;
    }
    tlen = strlen(tag);
    if (tlen + 3 >= sizeof(open)) {
        return -1;
    }
    open[0] = '<';
    memcpy(open + 1, tag, tlen);
    open[1 + tlen] = '>';
    open[2 + tlen] = '\0';
    olen = tlen + 2;
    for (i = 0; i + olen < len; i++) {
        if (memcmp(xml + i, open, olen) == 0) {
            size_t start = i + olen;
            size_t j = start;
            char close[72];
            size_t clen;
            snprintf(close, sizeof(close), "</%s>", tag);
            clen = strlen(close);
            while (j + clen <= len) {
                if (memcmp(xml + j, close, clen) == 0) {
                    size_t n = j - start;
                    if (n >= out_sz) {
                        n = out_sz - 1;
                    }
                    memcpy(out, xml + start, n);
                    out[n] = '\0';
                    return 0;
                }
                j++;
            }
            return -1;
        }
    }
    return -1;
}

static int xml_has_tag(const char *xml, size_t len, const char *tag)
{
    char open[64];
    size_t tlen, olen, i;
    tlen = strlen(tag);
    if (tlen + 2 >= sizeof(open)) {
        return 0;
    }
    open[0] = '<';
    memcpy(open + 1, tag, tlen);
    open[1 + tlen] = '\0';
    olen = tlen + 1;
    for (i = 0; i + olen <= len; i++) {
        if (memcmp(xml + i, open, olen) == 0) {
            char c = (i + olen < len) ? xml[i + olen] : '\0';
            if (c == '>' || c == ' ' || c == '/' || c == '\t' || c == '\n' ||
                c == '\r') {
                return 1;
            }
        }
    }
    return 0;
}

/**
 * Build net.pon key for lab.v1 notification (same patterns as apply).
 * @return 0 ok, -1 unknown/bad.
 */
static int lab_v1_state_key(const char *mac_colon, const char *xml, size_t len,
                            char *key, size_t key_sz)
{
    char mac[EDGE_E7_MAC_MAX];
    char mac_key[EDGE_E7_MAC_MAX];
    int n;

    if (!mac_colon || !xml || !key || key_sz == 0 || len == 0) {
        return -1;
    }
    if (edge_e7_mac_normalize(mac_colon, mac, sizeof(mac)) != 0 ||
        edge_e7_mac_to_key_seg(mac, mac_key, sizeof(mac_key)) != 0) {
        return -1;
    }
    if (xml_has_tag(xml, len, "ont-event")) {
        char ont_id[EDGE_E7_AID_MAX];
        char ont_key[EDGE_E7_AID_MAX];
        if (xml_tag_text(xml, len, "ont-id", ont_id, sizeof(ont_id)) != 0 ||
            edge_e7_aid_to_key_seg(ont_id, ont_key, sizeof(ont_key)) != 0) {
            return -1;
        }
        n = snprintf(key, key_sz, "e7/%s/ont/%s", mac_key, ont_key);
        return (n > 0 && (size_t)n < key_sz) ? 0 : -1;
    }
    if (xml_has_tag(xml, len, "pon-alarm")) {
        char pon_id[EDGE_E7_AID_MAX];
        char pon_key[EDGE_E7_AID_MAX];
        if (xml_tag_text(xml, len, "pon-id", pon_id, sizeof(pon_id)) != 0 ||
            edge_e7_aid_to_key_seg(pon_id, pon_key, sizeof(pon_key)) != 0) {
            return -1;
        }
        n = snprintf(key, key_sz, "e7/%s/pon/%s", mac_key, pon_key);
        return (n > 0 && (size_t)n < key_sz) ? 0 : -1;
    }
    return -1;
}

/**
 * map.dynamic key for ont-event when lon/lat (or longitude/latitude) present.
 * @return 0 if geo fields present and key built, -1 otherwise.
 */
static int lab_v1_map_key(const char *mac_colon, const char *xml, size_t len,
                          char *key, size_t key_sz)
{
    char mac[EDGE_E7_MAC_MAX];
    char mac_key[EDGE_E7_MAC_MAX];
    char ont_id[EDGE_E7_AID_MAX];
    char ont_key[EDGE_E7_AID_MAX];
    char lon_s[64];
    char lat_s[64];
    int n;

    if (!mac_colon || !xml || !key || key_sz == 0 || len == 0) {
        return -1;
    }
    if (!xml_has_tag(xml, len, "ont-event")) {
        return -1;
    }
    lon_s[0] = lat_s[0] = '\0';
    if (xml_tag_text(xml, len, "lon", lon_s, sizeof(lon_s)) != 0) {
        (void)xml_tag_text(xml, len, "longitude", lon_s, sizeof(lon_s));
    }
    if (xml_tag_text(xml, len, "lat", lat_s, sizeof(lat_s)) != 0) {
        (void)xml_tag_text(xml, len, "latitude", lat_s, sizeof(lat_s));
    }
    if (lon_s[0] == '\0' || lat_s[0] == '\0') {
        return -1;
    }
    if (edge_e7_mac_normalize(mac_colon, mac, sizeof(mac)) != 0 ||
        edge_e7_mac_to_key_seg(mac, mac_key, sizeof(mac_key)) != 0) {
        return -1;
    }
    if (xml_tag_text(xml, len, "ont-id", ont_id, sizeof(ont_id)) != 0 ||
        edge_e7_aid_to_key_seg(ont_id, ont_key, sizeof(ont_key)) != 0) {
        return -1;
    }
    n = snprintf(key, key_sz, "ont/%s/%s", mac_key, ont_key);
    return (n > 0 && (size_t)n < key_sz) ? 0 : -1;
}

static void e7_pcap_dial_summary(edge_e7_callhome_t *ch, edge_e7_session_t *s,
                                 const char *why);

static void session_clear_nc(edge_e7_session_t *s)
{
#if EDGEHOST_HAVE_LIBNETCONF
    if (s->nc) {
        netconf_destroy(s->nc);
        s->nc = NULL;
    }
#endif
#if EDGEHOST_E7_CHSSH_AVAILABLE
    if (s->chssh) {
        chssh_destroy(s->chssh);
        s->chssh = NULL;
    }
    s->use_chssh = 0;
    s->chssh_ready = 0;
#endif
#if !EDGEHOST_HAVE_LIBNETCONF && !EDGEHOST_E7_CHSSH_AVAILABLE
    (void)s;
#endif
}

static void session_close(edge_e7_callhome_t *ch, edge_e7_session_t *s)
{
    int was_open;
    const char *mac;
    const char *peer;
    const char *stname;

    if (!s || s->state == EDGE_E7_SESS_EMPTY) {
        return;
    }
    was_open = (s->state == EDGE_E7_SESS_OPEN);
    mac = (s->identity.identity_ok && s->identity.mac[0]) ? s->identity.mac
                                                          : "";
    peer = s->peer[0] ? s->peer : "";
    stname = edge_e7_sess_state_name(s->state);
    /* Capture dial summary before tearing down counters / fd. */
    if (s->identity_ms && s->cap_ident_ms == 0) {
        s->cap_ident_ms = s->identity_ms;
        s->cap_saw_ident = 1;
    }
    e7_pcap_dial_summary(ch, s, stname);
    e7_trace(ch, "closed", mac, peer, "from_state=%s", stname);
    /* Disconnect of an open session → inventory status + immediate notify. */
    if (was_open && s->identity.identity_ok) {
        put_session_json(ch, s, "disconnected");
    }
    if (was_open && ch->stats.sessions_open > 0) {
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
    s->sub_sent = 0;
    s->sub_ok = 0;
    s->sub_msg_id = 0;
    s->hello_sent = 0;
    s->use_ssh = 0;
    s->use_chssh = 0;
    s->chssh_ready = 0;
    s->nc_as_server = 0;
    s->ssh_banner_flushed = 0;
    s->saw_ssh_rx = 0;
    s->probe_mode = 0;
    s->probe_spoke = 0;
    s->probe_ack_sent = 0;
    s->ssh_phase = 0;
    s->ssh_resume = 0;
    s->first_tx_logged = 0;
    s->last_nc_state = -1;
    s->cap_t0_ms = 0;
    s->cap_ident_ms = 0;
    s->cap_first_tx_ms = 0;
    s->cap_rx_bytes = 0;
    s->cap_tx_bytes = 0;
    s->cap_rx_chunks = 0;
    s->cap_tx_chunks = 0;
    s->cap_saw_ident = 0;
    s->cap_saw_ssh_tx = 0;
    s->cap_saw_ack_tx = 0;
    s->cap_saw_ssh_rx = 0;
    s->cap_summary_done = 0;
    s->cap_first_tx_tag[0] = '\0';
    memset(&s->identity, 0, sizeof(s->identity));
    s->peer[0] = '\0';
}

static const char *e7_probe_mode_name(uint8_t mode)
{
    switch (mode) {
    case EDGE_E7_PROBE_SILENT:       return "silent_listen";
    case EDGE_E7_PROBE_VERSION_ACK:  return "version_ack";
    case EDGE_E7_PROBE_VERSION_ACK2: return "version_ack_alt";
    case EDGE_E7_PROBE_NC_CLIENT:    return "netconf_client_hello";
    case EDGE_E7_PROBE_NC_SERVER:    return "netconf_server_hello";
    default:                         return "unknown";
    }
}

static int session_append_tx(edge_e7_session_t *s, const uint8_t *data,
                             size_t n);
static int session_flush_tx(edge_e7_callhome_t *ch, edge_e7_session_t *s);

static void probe_advance_on_fail(edge_e7_callhome_t *ch, edge_e7_session_t *s)
{
    edge_e7_runtime_shelf_t *rs;
    uint8_t next;

    if (!ch || !s || !s->identity.identity_ok) {
        return;
    }
    rs = runtime_find(ch, s->identity.mac);
    if (!rs) {
        return;
    }
    next = (uint8_t)((rs->probe_mode + 1u) % (uint8_t)EDGE_E7_PROBE_COUNT);
    e7_trace(ch, "probe_advance", s->identity.mac, s->peer,
             "mode %u (%s) failed before OPEN → next dial mode %u (%s)",
             (unsigned)rs->probe_mode, e7_probe_mode_name(rs->probe_mode),
             (unsigned)next, e7_probe_mode_name(next));
    rs->probe_mode = next;
}

static int session_send_raw(edge_e7_callhome_t *ch, edge_e7_session_t *s,
                            const void *data, size_t len, const char *what)
{
    if (!s || s->fd < 0 || !data || len == 0) {
        return -1;
    }
    if (session_append_tx(s, (const uint8_t *)data, len) != 0) {
        return -1;
    }
    s->probe_spoke = 1;
    if (!s->first_tx_logged) {
        s->first_tx_logged = 1;
        e7_trace_bytes(ch, "first_tx", s->identity.mac, s->peer,
                       what ? what : "raw", data, len);
    } else {
        e7_trace_bytes(ch, "probe_tx", s->identity.mac, s->peer,
                       what ? what : "raw", data, len);
    }
    if (session_flush_tx(ch, s) != 0) {
        e7_trace(ch, "write_err", s->identity.mac, s->peer,
                 "flush %s errno=%d", what ? what : "bytes", errno);
        return -1;
    }
    e7_trace(ch, "probe_tx", s->identity.mac, s->peer, "sent %s (%zu bytes)",
             what ? what : "data", len);
    return 0;
}

#if EDGEHOST_HAVE_LIBNETCONF
static int session_begin_after_identity(edge_e7_callhome_t *ch,
                                        edge_e7_session_t *s, int use_ssh);
#endif

/** Send proprietary Calix-style identity ACK (not NETCONF hello). */
static int session_send_version_ack(edge_e7_callhome_t *ch, edge_e7_session_t *s,
                                    int alt)
{
    /* Observed E7 waits for NMS after identity; NETCONF hello is rejected.
     * Try identity-layer ACKs that mirror the preamble's <version> framing. */
    static const char ack0[] = "<version>1</version>\n";
    static const char ack1[] = "<version>1</version>";
    static const char ack2[] = "<version>1</version>\r\n";
    static const char ack3[] = "<?xml version=\"1.0\"?>\n<version>1</version>\n";
    const char *ack;
    size_t len;
    const char *tag;

    if (alt) {
        /* Rotate alts using probe_ack_sent as a small counter */
        switch (s->probe_ack_sent % 3) {
        case 0:
            ack = ack1;
            len = sizeof(ack1) - 1;
            tag = "version ACK (no NL)";
            break;
        case 1:
            ack = ack2;
            len = sizeof(ack2) - 1;
            tag = "version ACK (CRLF)";
            break;
        default:
            ack = ack3;
            len = sizeof(ack3) - 1;
            tag = "version ACK (xml decl)";
            break;
        }
    } else {
        ack = ack0;
        len = sizeof(ack0) - 1;
        tag = "version ACK (LF)";
    }
    s->probe_ack_sent = (uint8_t)(s->probe_ack_sent + 1);
    return session_send_raw(ch, s, ack, len, tag);
}

/**
 * Field SSH ladder: identity-layer unlock candidates before SSH banner.
 * Each form is tried once per phase on the same dial (after silent hold).
 * @return 0 ok, -1 write failed.
 */
static int __attribute__((unused)) session_send_ssh_field_ack(edge_e7_callhome_t *ch,
                                      edge_e7_session_t *s, unsigned phase)
{
    /* phase EDGE_E7_SSH_PHASE_ACK0..ACK4 */
    static const char ack_lf[] = "<version>1</version>\n";
    static const char ack_crlf[] = "<version>1</version>\r\n";
    static const char ack_ok[] = "OK\n";
    static const char ack_nl[] = "\n";
    static const char ack_id[] =
        "<identity-response><status>ok</status></identity-response>\n";
    const char *ack = NULL;
    size_t len = 0;
    const char *tag = "identity ACK";

    switch (phase) {
    case EDGE_E7_SSH_PHASE_ACK0:
        ack = ack_lf;
        len = sizeof(ack_lf) - 1;
        tag = "identity ACK version+LF";
        break;
    case EDGE_E7_SSH_PHASE_ACK1:
        ack = ack_crlf;
        len = sizeof(ack_crlf) - 1;
        tag = "identity ACK version+CRLF";
        break;
    case EDGE_E7_SSH_PHASE_ACK2:
        ack = ack_ok;
        len = sizeof(ack_ok) - 1;
        tag = "identity ACK OK\\n";
        break;
    case EDGE_E7_SSH_PHASE_ACK3:
        ack = ack_nl;
        len = sizeof(ack_nl) - 1;
        tag = "identity ACK bare LF";
        break;
    case EDGE_E7_SSH_PHASE_ACK4:
        ack = ack_id;
        len = sizeof(ack_id) - 1;
        tag = "identity ACK identity-response";
        break;
    default:
        return -1;
    }
    e7_trace(ch, "ssh_field_ack", s->identity.mac, s->peer,
             "phase=%u %s (%zu bytes)", phase, tag, len);
    return session_send_raw(ch, s, ack, len, tag);
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
#if EDGEHOST_E7_CHSSH_AVAILABLE
        /*
         * Over libchssh the NETCONF app stream rides the SSH channel —
         * never write cleartext hello/RPC onto the Call Home TCP socket.
         */
        if (s->use_chssh && s->chssh && s->chssh_ready) {
            if (chssh_channel_send(s->chssh, tmp, n) != 0) {
                return -1;
            }
            continue;
        }
#endif
        if (session_append_tx(s, tmp, n) != 0) {
            return -1;
        }
    }
    return 0;
}

static void session_try_subscribe(edge_e7_callhome_t *ch, edge_e7_session_t *s)
{
    int mid;
    const char *stream = "exa-events";

    if (!s || !s->nc || s->sub_sent || s->state != EDGE_E7_SESS_OPEN) {
        return;
    }
    /* Allowlisted or accepted via auto_subscribe_unknown. */
    if (!s->allowlisted && !s->auto_unknown) {
        return;
    }
    /*
     * Calix field create-subscription (compact form on wire after wrap):
     *   <create-subscription xmlns="...notification:1.0">
     *     <stream>exa-events</stream>
     *   </create-subscription>
     * Override via plugins.e7_callhome.subscription_stream if needed.
     */
    if (ch && ch->cfg && ch->cfg->e7_subscription_stream[0]) {
        stream = ch->cfg->e7_subscription_stream;
    }
    mid = netconf_create_subscription(s->nc, stream, NULL);
    if (mid < 0) {
        e7_trace(ch, "subscribe_fail",
                 s->identity.identity_ok ? s->identity.mac : "", s->peer,
                 "create_subscription stream=%s failed", stream);
        return;
    }
    s->sub_sent = 1;
    s->sub_msg_id = mid;
    e7_trace(ch, "subscribe", s->identity.identity_ok ? s->identity.mac : "",
             s->peer, "create-subscription stream=%s msg_id=%d", stream, mid);
    fprintf(stderr,
            "edgehost: e7_subscribe mac=%s peer=%s stream=%s msg_id=%d\n",
            s->identity.identity_ok ? s->identity.mac : "-",
            s->peer[0] ? s->peer : "-", stream, mid);
    (void)session_drain_output(s);
}

/**
 * One-line printable peek of notification XML for process / SPA logs.
 * Newlines → space; non-printables → '.'; truncates with "...".
 */
static void e7_event_xml_peek(const char *xml, size_t xml_len, char *out,
                              size_t out_sz)
{
    size_t i, o = 0;
    size_t max_body;

    if (!out || out_sz < 5) {
        return;
    }
    out[0] = '\0';
    if (!xml || xml_len == 0) {
        return;
    }
    /* Reserve 3 for "..." + NUL when truncating. */
    max_body = out_sz - 4;
    for (i = 0; i < xml_len && o < max_body; i++) {
        unsigned char c = (unsigned char)xml[i];
        if (c == '\n' || c == '\r' || c == '\t') {
            c = ' ';
        } else if (c < 32 || c >= 127) {
            c = '.';
        }
        out[o++] = (char)c;
    }
    if (i < xml_len) {
        out[o++] = '.';
        out[o++] = '.';
        out[o++] = '.';
    }
    out[o] = '\0';
}

#if EDGEHOST_HAVE_LIBYANG
/** Read entire file into malloc buffer; caller frees. */
static char *e7_read_file(const char *path, size_t *out_len)
{
    FILE *f;
    long sz;
    char *buf;
    if (!path || !path[0]) {
        return NULL;
    }
    f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[sz] = '\0';
    if (out_len) {
        *out_len = (size_t)sz;
    }
    return buf;
}

static int e7_yang_try_load_path(edge_e7_callhome_t *ch, const char *path)
{
    char *text;
    size_t len = 0;
    int rc;
    if (!ch || !ch->yang || !path) {
        return -1;
    }
    text = e7_read_file(path, &len);
    if (!text) {
        return -1;
    }
    rc = yang_load_module_text(ch->yang, text, len);
    free(text);
    if (rc == 0) {
        e7_trace(ch, "yang_loaded", "", "", "loaded %s", path);
        fprintf(stderr, "edgehost: yang_loaded path=%s\n", path);
    }
    return rc;
}

/** Try cache filenames for a module id. */
static int e7_yang_try_load_module(edge_e7_callhome_t *ch,
                                   const yang_module_id_t *id)
{
    char path[1024];
    int n;
    if (!ch || !id || !id->name[0]) {
        return -1;
    }
    if (id->revision[0]) {
        n = snprintf(path, sizeof(path), "%s/%s@%s.yang", ch->yang_cache_dir,
                     id->name, id->revision);
        if (n > 0 && (size_t)n < sizeof(path) &&
            e7_yang_try_load_path(ch, path) == 0) {
            return 0;
        }
    }
    n = snprintf(path, sizeof(path), "%s/%s.yang", ch->yang_cache_dir,
                 id->name);
    if (n > 0 && (size_t)n < sizeof(path) &&
        e7_yang_try_load_path(ch, path) == 0) {
        return 0;
    }
    /* Sibling libyang test fixtures (dev). */
    n = snprintf(path, sizeof(path),
                 "/home/dwhite/libyang/tests/fixtures/%s-mini.yang", id->name);
    if (n > 0 && (size_t)n < sizeof(path) &&
        e7_yang_try_load_path(ch, path) == 0) {
        return 0;
    }
    return -1;
}

static void e7_yang_drain(edge_e7_callhome_t *ch, const char *mac,
                          const char *peer)
{
    yang_event_t yev;
    if (!ch || !ch->yang) {
        return;
    }
    while (yang_next_event(ch->yang, &yev)) {
        if (yev.type == YANG_EVENT_MODULE_NEEDED) {
            e7_trace(ch, "yang_needed", mac ? mac : "", peer ? peer : "",
                     "module=%s revision=%s ns=%s", yev.u.needed.name,
                     yev.u.needed.revision[0] ? yev.u.needed.revision : "-",
                     yev.u.needed.namespace_uri[0] ? yev.u.needed.namespace_uri
                                                   : "-");
            fprintf(stderr,
                    "edgehost: yang_needed module=%s revision=%s\n",
                    yev.u.needed.name,
                    yev.u.needed.revision[0] ? yev.u.needed.revision : "-");
            if (e7_yang_try_load_module(ch, &yev.u.needed) != 0) {
                e7_trace(ch, "yang_missing", mac ? mac : "", peer ? peer : "",
                         "no cache for %s@%s (host get-schema later)",
                         yev.u.needed.name,
                         yev.u.needed.revision[0] ? yev.u.needed.revision : "");
            }
        } else if (yev.type == YANG_EVENT_MODULE_LOADED) {
            e7_trace(ch, "yang_loaded", mac ? mac : "", peer ? peer : "",
                     "module=%s revision=%s", yev.u.loaded.name,
                     yev.u.loaded.revision[0] ? yev.u.loaded.revision : "-");
        } else if (yev.type == YANG_EVENT_SCHEMA_READY) {
            e7_trace(ch, "yang_ready", mac ? mac : "", peer ? peer : "",
                     "modules_loaded=%zu pending=%zu",
                     yev.u.ready.modules_loaded, yev.u.ready.modules_pending);
            fprintf(stderr,
                    "edgehost: yang_ready modules=%zu pending=%zu\n",
                    yev.u.ready.modules_loaded, yev.u.ready.modules_pending);
        } else if (yev.type == YANG_EVENT_MODULE_ERROR) {
            e7_trace(ch, "yang_error", mac ? mac : "", peer ? peer : "", "%s",
                     yev.u.module_error.message);
        }
    }
}

static void e7_yang_on_hello(edge_e7_callhome_t *ch, edge_e7_session_t *s,
                             const netconf_hello_t *hello)
{
    const char *uris[NETCONF_MAX_CAPABILITIES];
    size_t n = 0, i;
    const char *mac;
    const char *peer;
    if (!ch || !ch->yang || !hello) {
        return;
    }
    mac = (s && s->identity.identity_ok) ? s->identity.mac : "";
    peer = (s && s->peer[0]) ? s->peer : "";
    for (i = 0; i < hello->capability_count && n < NETCONF_MAX_CAPABILITIES;
         i++) {
        if (hello->capabilities[i].uri[0]) {
            uris[n++] = hello->capabilities[i].uri;
        }
    }
    e7_trace(ch, "yang_hello", mac, peer, "observing %zu module-capable URIs",
             n);
    fprintf(stderr, "edgehost: yang_hello mac=%s caps=%zu\n",
            mac[0] ? mac : "-", n);
    (void)yang_observe_capabilities(ch->yang, uris, n);
    /* Drain MODULE_NEEDED → try cache load → LOADED / SCHEMA_READY. */
    e7_yang_drain(ch, mac, peer);
}

/**
 * Log notification as hierarchical path=value lines (schema-aware when loaded).
 */
static void e7_yang_log_paths(edge_e7_callhome_t *ch, edge_e7_session_t *s,
                              const netconf_notification_t *n)
{
    yang_path_value_t leaves[YANG_DECODE_MAX_LEAVES];
    size_t count = 0, i;
    char notif[YANG_NAME_MAX];
    char mod[YANG_NAME_MAX];
    char etime[64];
    const char *mac;
    const char *peer;
    const yang_ctx_t *yctx;

    if (!ch || !n || n->xml_len == 0) {
        return;
    }
    mac = (s && s->identity.identity_ok) ? s->identity.mac : "";
    peer = (s && s->peer[0]) ? s->peer : "";
    yctx = ch->yang;
    if (yang_decode_notification(yctx, n->xml, n->xml_len, leaves,
                                 YANG_DECODE_MAX_LEAVES, &count, notif,
                                 sizeof(notif), mod, sizeof(mod), etime,
                                 sizeof(etime)) != 0) {
        return;
    }
    e7_trace(ch, "event_paths", mac, peer,
             "notif=%s module=%s eventTime=%s leaves=%zu",
             notif[0] ? notif : "?", mod[0] ? mod : "-",
             etime[0] ? etime : "-", count);
    fprintf(stderr,
            "edgehost: e7_event_paths mac=%s notif=%s module=%s eventTime=%s "
            "leaves=%zu\n",
            mac[0] ? mac : "-", notif[0] ? notif : "?", mod[0] ? mod : "-",
            etime[0] ? etime : "-", count);
    if (ch->pcap_text_fp) {
        fprintf(ch->pcap_text_fp,
                "# PATHS mac=%s notif=%s module=%s eventTime=%s leaves=%zu\n",
                mac[0] ? mac : "-", notif[0] ? notif : "?",
                mod[0] ? mod : "-", etime[0] ? etime : "-", count);
    }
    for (i = 0; i < count; i++) {
        e7_trace(ch, "event_path", mac, peer, "%s = %s", leaves[i].path,
                 leaves[i].value);
        fprintf(stderr, "edgehost: e7_path mac=%s %s = %s\n",
                mac[0] ? mac : "-", leaves[i].path, leaves[i].value);
        if (ch->pcap_text_fp) {
            fprintf(ch->pcap_text_fp, "# PATH %s = %s\n", leaves[i].path,
                    leaves[i].value);
        }
    }
    if (ch->pcap_text_fp) {
        fflush(ch->pcap_text_fp);
    }
}

static void e7_yang_init(edge_e7_callhome_t *ch)
{
    yang_config_t ycfg;
    if (!ch) {
        return;
    }
    memset(&ycfg, 0, sizeof(ycfg));
    ch->yang = yang_create(&ycfg);
    snprintf(ch->yang_cache_dir, sizeof(ch->yang_cache_dir), "var/yang");
    if (!ch->yang) {
        fprintf(stderr, "edgehost: yang_create failed\n");
        return;
    }
    /* Preload common fixtures from cache if present. */
    {
        static const char *seed[] = {
            "calix-exa-base@2020-01-01.yang",
            "calix-exa-base.yang",
            "ietf-interfaces@2018-02-20.yang",
            "ietf-interfaces.yang",
            NULL};
        size_t i;
        char path[640];
        for (i = 0; seed[i]; i++) {
            snprintf(path, sizeof(path), "%s/%s", ch->yang_cache_dir, seed[i]);
            (void)e7_yang_try_load_path(ch, path);
        }
        e7_yang_drain(ch, "", "");
    }
    fprintf(stderr, "edgehost: libyang ready cache=%s modules=%zu\n",
            ch->yang_cache_dir, yang_module_count(ch->yang));
}
#endif /* EDGEHOST_HAVE_LIBYANG */

static void on_notification(edge_e7_callhome_t *ch, edge_e7_session_t *s,
                            const netconf_notification_t *n)
{
    edge_e7_apply_err_t ae;
    char key[EDGE_STATE_KEY_MAX];
    char peek[160];
    const char *mac;
    const char *peer;
    const char *etime;

    if (!ch || !s || !n || n->xml_len == 0) {
        return;
    }
    ch->stats.notifications++;
    mac = s->identity.identity_ok ? s->identity.mac : "";
    peer = s->peer[0] ? s->peer : "";
    etime = (n->event_time_len > 0 && n->event_time[0]) ? n->event_time : "-";
    e7_event_xml_peek(n->xml, n->xml_len, peek, sizeof(peek));

    /* SPA ring + process log: every notification, before apply. */
    e7_trace(ch, "event", mac, peer, "notification %zuB eventTime=%s %s",
             n->xml_len, etime, peek);
    fprintf(stderr,
            "edgehost: e7_event mac=%s peer=%s eventTime=%s len=%zu xml=%s\n",
            mac[0] ? mac : "-", peer[0] ? peer : "-", etime, n->xml_len, peek);

    /* Full body in host stream text capture when enabled. */
    if (ch->pcap_text_fp) {
        fprintf(ch->pcap_text_fp,
                "# EVENT mac=%s peer=%s eventTime=%s len=%zu\n",
                mac[0] ? mac : "-", peer[0] ? peer : "-", etime, n->xml_len);
        fwrite(n->xml, 1, n->xml_len, ch->pcap_text_fp);
        if (n->xml_len == 0 || n->xml[n->xml_len - 1] != '\n') {
            fputc('\n', ch->pcap_text_fp);
        }
        fflush(ch->pcap_text_fp);
    }

#if EDGEHOST_HAVE_LIBYANG
    /* Hierarchical path form (primary operator view). */
    e7_yang_log_paths(ch, s, n);
#endif

    if (!ch->state || !s->identity.identity_ok) {
        return;
    }
    {
        char source[16];
        source[0] = '\0';
        ae = edge_e7_event_apply(ch->state, s->identity.mac, n->xml, n->xml_len,
                                 key, sizeof(key), source, sizeof(source));
        if (ae != EDGE_E7_APPLY_OK) {
            e7_trace(ch, "event_apply", mac, peer,
                     "apply fail %s (%d) — raw event logged; no state put",
                     edge_e7_apply_err_name(ae), (int)ae);
            fprintf(stderr,
                    "edgehost: e7_event_apply mac=%s fail=%s code=%d\n",
                    mac[0] ? mac : "-", edge_e7_apply_err_name(ae), (int)ae);
            return;
        }
        ch->stats.state_puts++;
        e7_trace(ch, "event_apply", mac, peer, "applied source=%s key=%s",
                 source[0] ? source : "?", key[0] ? key : "-");
        fprintf(stderr, "edgehost: e7_event_apply mac=%s source=%s key=%s\n",
                mac[0] ? mac : "-", source[0] ? source : "?",
                key[0] ? key : "-");
        if (key[0]) {
            notify_coalesce(ch, "net.pon", key);
        } else if (lab_v1_state_key(s->identity.mac, n->xml, n->xml_len, key,
                                    sizeof(key)) == 0) {
            notify_coalesce(ch, "net.pon", key);
        }
        /* PR-9: map.dynamic ONT mirror when lab.v1 geo present. */
        if (lab_v1_map_key(s->identity.mac, n->xml, n->xml_len, key,
                           sizeof(key)) == 0) {
            notify_coalesce(ch, "map.dynamic", key);
        }
    }
}

static void command_store_result(edge_e7_callhome_t *ch, edge_e7_cmd_slot_t *cmd,
                                 const netconf_rpc_reply_t *reply)
{
    char mac_key[EDGE_E7_MAC_MAX];
    char key[EDGE_STATE_KEY_MAX];
    char json[EDGE_STATE_VALUE_DEFAULT];
    int n;
    int ok;
    edge_state_err_t e;
    size_t xml_copy;
    size_t max_xml;

    if (!ch || !cmd || !reply) {
        return;
    }
    ok = reply->is_ok && !reply->has_errors;
    if (edge_e7_mac_to_key_seg(cmd->mac, mac_key, sizeof(mac_key)) != 0) {
        return;
    }
    n = snprintf(key, sizeof(key), "e7/%s/cmd/%s", mac_key, cmd->cmd_id);
    if (n < 0 || (size_t)n >= sizeof(key)) {
        return;
    }
    /* Compact result; truncate xml if needed for value budget. */
    max_xml = sizeof(json) > 256 ? sizeof(json) - 200 : 64;
    xml_copy = reply->xml_len < max_xml ? reply->xml_len : max_xml;
    n = snprintf(json, sizeof(json),
                 "{\"v\":1,\"cmd_id\":\"%s\",\"mac\":\"%s\",\"message_id\":%u,"
                 "\"status\":\"%s\",\"is_ok\":%s,\"has_errors\":%s,"
                 "\"xml_len\":%zu,\"xml\":",
                 cmd->cmd_id, cmd->mac, (unsigned)cmd->message_id,
                 ok ? "ok" : "error", ok ? "true" : "false",
                 reply->has_errors ? "true" : "false", reply->xml_len);
    if (n < 0 || (size_t)n + xml_copy + 4 >= sizeof(json)) {
        n = snprintf(json, sizeof(json),
                     "{\"v\":1,\"cmd_id\":\"%s\",\"mac\":\"%s\",\"message_id\":%u,"
                     "\"status\":\"%s\",\"xml_truncated\":true}",
                     cmd->cmd_id, cmd->mac, (unsigned)cmd->message_id,
                     ok ? "ok" : "error");
    } else {
        /* Append JSON string of xml (escape minimal: no quotes expected often) */
        size_t pos = (size_t)n;
        size_t i;
        json[pos++] = '"';
        for (i = 0; i < xml_copy && pos + 2 < sizeof(json); i++) {
            char c = reply->xml[i];
            if (c == '"' || c == '\\') {
                if (pos + 3 >= sizeof(json)) {
                    break;
                }
                json[pos++] = '\\';
            }
            if ((unsigned char)c < 0x20) {
                continue;
            }
            json[pos++] = c;
        }
        if (pos + 2 < sizeof(json)) {
            json[pos++] = '"';
            json[pos++] = '}';
            json[pos] = '\0';
            n = (int)pos;
        }
    }
    if (n < 0) {
        return;
    }
    e = edge_state_put_and_notify(ch->state, ch->hub, "net.pon", key, json,
                                  (size_t)n, NULL, 0);
    if (e == EDGE_STATE_OK) {
        ch->stats.state_puts++;
        ch->stats.ws_fanouts++;
    }
    cmd->complete = 1;
    if (ok) {
        ch->stats.commands_ok++;
    } else {
        ch->stats.commands_err++;
    }
}

static void on_rpc_reply(edge_e7_callhome_t *ch, edge_e7_session_t *s,
                         const netconf_rpc_reply_t *reply)
{
    uint32_t i;

    if (!ch || !s || !reply) {
        return;
    }
    if (s->sub_sent && !s->sub_ok && s->sub_msg_id > 0 &&
        reply->message_id == (uint32_t)s->sub_msg_id) {
        s->sub_ok = 1;
        ch->stats.subscriptions_ok++;
        e7_trace(ch, "subscribed",
                 s->identity.identity_ok ? s->identity.mac : "", s->peer,
                 "create-subscription ok msg_id=%u",
                 (unsigned)reply->message_id);
    }
    for (i = 0; i < EDGE_E7_CMD_TABLE_MAX; i++) {
        edge_e7_cmd_slot_t *cmd = &ch->cmds[i];
        if (!cmd->used || cmd->complete) {
            continue;
        }
        if (cmd->shelf_slot == s->slot &&
            cmd->message_id == reply->message_id) {
            command_store_result(ch, cmd, reply);
            break;
        }
    }
}

static void drain_nc_events(edge_e7_callhome_t *ch, edge_e7_session_t *s)
{
    netconf_event_t ev;
    while (s->nc && netconf_next_event(s->nc, &ev) == 1) {
        if (ev.type == NETCONF_EVENT_NOTIFICATION) {
            on_notification(ch, s, &ev.data.notification);
        } else if (ev.type == NETCONF_EVENT_HELLO_RECEIVED) {
#if EDGEHOST_HAVE_LIBYANG
            e7_yang_on_hello(ch, s, &ev.data.hello);
#else
            e7_trace(ch, "hello_rx",
                     s->identity.identity_ok ? s->identity.mac : "", s->peer,
                     "peer hello caps=%zu (libyang not linked)",
                     ev.data.hello.capability_count);
#endif
        } else if (ev.type == NETCONF_EVENT_RPC_OK ||
                   ev.type == NETCONF_EVENT_RPC_REPLY) {
            on_rpc_reply(ch, s, &ev.data.rpc_reply);
        } else if (ev.type == NETCONF_EVENT_SSH_AUTHENTICATED) {
            e7_trace(ch, "ssh_auth_ok",
                     s->identity.identity_ok ? s->identity.mac : "", s->peer,
                     "SSH userauth succeeded");
        } else if (ev.type == NETCONF_EVENT_SSH_CONNECTED) {
            e7_trace(ch, "ssh_channel",
                     s->identity.identity_ok ? s->identity.mac : "", s->peer,
                     "netconf subsystem channel ready");
        } else if (ev.type == NETCONF_EVENT_SSH_DISCONNECTED) {
            e7_trace(ch, "ssh_disconnect",
                     s->identity.identity_ok ? s->identity.mac : "", s->peer,
                     "SSH disconnected");
        } else if (ev.type == NETCONF_EVENT_SSH_ERROR) {
            e7_trace(ch, "ssh_error",
                     s->identity.identity_ok ? s->identity.mac : "", s->peer,
                     "libnetconf SSH error (algo/auth/hostkey?)");
        } else if (ev.type == NETCONF_EVENT_ERROR) {
            e7_trace(ch, "nc_error",
                     s->identity.identity_ok ? s->identity.mac : "", s->peer,
                     "%s",
                     ev.data.error.message[0] ? ev.data.error.message
                                              : "NETCONF_EVENT_ERROR");
        }
        /* QUEUE_OVERFLOW: count later */
    }
}

#if EDGEHOST_E7_SSH_AVAILABLE && !EDGEHOST_E7_CHSSH_AVAILABLE
/**
 * Legacy: libassh-in-libnetconf SSH roles (used only when libchssh absent).
 */
static void edge_e7_netconf_apply_ssh(netconf_config_t *ncfg,
                                      const edge_config_t *cfg)
{
    if (!ncfg || !cfg) {
        return;
    }
    ncfg->ssh_mode = NETCONF_SSH_CALLHOME;
    ncfg->ssh_server_password =
        cfg->e7_ssh_password[0] ? cfg->e7_ssh_password : "sysadmin";
    ncfg->ssh_server_username =
        cfg->e7_ssh_username[0] ? cfg->e7_ssh_username : "sysadmin";
    ncfg->ssh_allow_none_auth = cfg->e7_ssh_allow_none_auth ? 1 : 0;
    if (cfg->e7_host_key_path[0]) {
        ncfg->host_key_path = cfg->e7_host_key_path;
    } else {
        ncfg->host_key_path = NULL;
    }
    ncfg->client_username = NULL;
    ncfg->client_password = NULL;
    ncfg->ssh_accept_any_hostkey = 0;
}
#endif

static void session_trace_nc_state(edge_e7_callhome_t *ch, edge_e7_session_t *s);
static int session_flush_tx(edge_e7_callhome_t *ch, edge_e7_session_t *s);
static int session_drain_output(edge_e7_session_t *s);
static void session_after_nc_io(edge_e7_callhome_t *ch, edge_e7_session_t *s);
static void e7_pcap_write(edge_e7_callhome_t *ch, edge_e7_session_t *s,
                          int to_peer, const uint8_t *data, size_t len);
static void e7_pcap_dial_summary(edge_e7_callhome_t *ch, edge_e7_session_t *s,
                                 const char *why);
#if EDGEHOST_HAVE_LIBNETCONF
static int session_begin_after_identity(edge_e7_callhome_t *ch,
                                        edge_e7_session_t *s, int use_ssh);
#endif

/**
 * Apply a conservative capability set for field gear (Calix E7).
 * Advertising base:1.1 without RFC 6242 chunked framing causes some peers to
 * abort as soon as they parse our hello — use base:1.0 only for lab raw.
 */
static void session_apply_e7_caps(edge_e7_callhome_t *ch, edge_e7_session_t *s)
{
    netconf_capability_t caps[2];
    static const char *uris[] = {
        "urn:ietf:params:netconf:base:1.0",
    };
    size_t i, n = sizeof(uris) / sizeof(uris[0]);

    (void)ch;
    if (!s || !s->nc) {
        return;
    }
    memset(caps, 0, sizeof(caps));
    for (i = 0; i < n; i++) {
        size_t len = strlen(uris[i]);
        if (len >= sizeof(caps[i].uri)) {
            continue;
        }
        memcpy(caps[i].uri, uris[i], len);
        caps[i].uri_len = len;
    }
    if (netconf_set_capabilities(s->nc, caps, n) != 0) {
        e7_trace(ch, "caps_fail", s->identity.identity_ok ? s->identity.mac : "",
                 s->peer, "netconf_set_capabilities failed; using library defaults");
    } else {
        e7_trace(ch, "caps", s->identity.identity_ok ? s->identity.mac : "",
                 s->peer, "hello caps: base:1.0 only");
    }
}

/**
 * After SSH subsystem is ready (CAPABILITY_EXCHANGE), send client hello.
 * @return 0 ok / still waiting, -1 fatal.
 */
static int session_try_send_hello(edge_e7_callhome_t *ch, edge_e7_session_t *s)
{
    netconf_state_t st;

    if (!s || !s->nc || s->hello_sent) {
        return 0;
    }
    st = netconf_current_state(s->nc);
    if (st == NETCONF_STATE_ERROR || st == NETCONF_STATE_CLOSED ||
        st == NETCONF_STATE_CLOSING) {
        return -1;
    }
    /* libchssh: wait until subsystem ready (nc created only then). */
    if (s->use_chssh && !s->chssh_ready) {
        return 0;
    }
    /* Legacy libassh-in-libnetconf: wait for subsystem. */
    if (s->use_ssh && !s->use_chssh && st != NETCONF_STATE_CAPABILITY_EXCHANGE &&
        st != NETCONF_STATE_SESSION_OPEN) {
        return 0; /* still SSH_CONNECTING / SSH_AUTHENTICATING */
    }
    session_apply_e7_caps(ch, s);
    if (netconf_send_hello(s->nc, NULL, 0) != 0) {
        return -1;
    }
    s->hello_sent = 1;
    s->state = EDGE_E7_SESS_HELLO;
    e7_trace(ch, "hello", s->identity.identity_ok ? s->identity.mac : "",
             s->peer,
             s->nc_as_server
                 ? "NETCONF server hello sent (session-id, base:1.0, ]]>]]>)"
                 : "NETCONF client hello sent (base:1.0, ]]>]]>)");
    drain_nc_events(ch, s);
    if (session_drain_output(s) != 0) {
        return -1;
    }
    return 0;
}

#if EDGEHOST_E7_CHSSH_AVAILABLE
/** Drain libchssh wire output into session TX buffer. */
static int session_drain_chssh(edge_e7_callhome_t *ch, edge_e7_session_t *s)
{
    uint8_t tmp[8192];
    size_t n;
    if (!s || !s->chssh) {
        return 0;
    }
    for (;;) {
        n = chssh_get_output(s->chssh, tmp, sizeof(tmp));
        if (n == 0) {
            break;
        }
        if (!s->first_tx_logged) {
            s->first_tx_logged = 1;
            e7_trace_bytes(ch, "first_tx",
                           s->identity.identity_ok ? s->identity.mac : "",
                           s->peer, "SSH (libchssh)", tmp, n);
        }
        if (session_append_tx(s, tmp, n) != 0) {
            return -1;
        }
    }
    return 0;
}

/** Start raw NETCONF CLIENT over ready chssh channel. */
static int session_start_nc_over_chssh(edge_e7_callhome_t *ch,
                                       edge_e7_session_t *s)
{
    netconf_config_t ncfg;
    if (!s || s->nc) {
        return 0;
    }
    edge_e7_netconf_profile(&ncfg);
    ncfg.ssh_mode = NETCONF_SSH_OFF;
    s->nc = netconf_create_with_config(NETCONF_ROLE_CLIENT, &ncfg);
    if (!s->nc) {
        e7_trace(ch, "netconf_create_fail", s->identity.mac, s->peer,
                 "netconf over chssh create failed");
        return -1;
    }
    e7_trace(ch, "nc_role", s->identity.mac, s->peer,
             "NETCONF_ROLE=CLIENT ssh_mode=OFF (over libchssh netconf channel)");
    if (session_try_send_hello(ch, s) != 0) {
        return -1;
    }
    return 0;
}

/**
 * Pull chssh events; on READY start NETCONF; on CHANNEL_DATA feed netconf.
 * Queue netconf output back into chssh channel.
 */
static int session_chssh_process(edge_e7_callhome_t *ch, edge_e7_session_t *s)
{
    chssh_event_t ev;
    if (!s || !s->chssh) {
        return 0;
    }
    while (chssh_next_event(s->chssh, &ev)) {
        switch (ev.type) {
        case CHSSH_EVENT_IDENT_SENT:
            s->ssh_banner_flushed = 1;
            e7_trace(ch, "ssh_banner_sent", s->identity.mac, s->peer,
                     "libchssh server identification flushed");
            break;
        case CHSSH_EVENT_IDENT_RECEIVED:
            s->saw_ssh_rx = 1;
            e7_trace(ch, "ssh_rx", s->identity.mac, s->peer,
                     "peer SSH ident \"%s\"", ev.u.ident.banner);
            break;
        case CHSSH_EVENT_AUTHENTICATED:
            e7_trace(ch, "ssh_auth", s->identity.mac, s->peer,
                     "SSH user authenticated (libchssh)");
            break;
        case CHSSH_EVENT_READY:
            s->chssh_ready = 1;
            e7_trace(ch, "ssh_ready", s->identity.mac, s->peer,
                     "libchssh subsystem=netconf READY — starting NETCONF client");
            if (session_start_nc_over_chssh(ch, s) != 0) {
                return -1;
            }
            break;
        case CHSSH_EVENT_CHANNEL_DATA:
            if (s->nc && ev.u.data.len > 0) {
                (void)netconf_feed_input(s->nc, ev.u.data.data, ev.u.data.len);
                drain_nc_events(ch, s);
                session_after_nc_io(ch, s);
                if (s->state == EDGE_E7_SESS_EMPTY) {
                    return -1;
                }
            }
            break;
        case CHSSH_EVENT_ERROR:
            e7_trace(ch, "ssh_error", s->identity.mac, s->peer, "%s",
                     ev.u.error.message);
            return -1;
        case CHSSH_EVENT_DISCONNECTED:
            e7_trace(ch, "ssh_disconnected", s->identity.mac, s->peer,
                     "libchssh disconnect");
            return -1;
        default:
            break;
        }
    }
    /* NETCONF app bytes → SSH channel */
    if (s->nc && s->chssh_ready) {
        uint8_t tmp[8192];
        size_t n;
        for (;;) {
            n = netconf_get_output(s->nc, tmp, sizeof(tmp));
            if (n == 0) {
                break;
            }
            if (chssh_channel_send(s->chssh, tmp, n) != 0) {
                e7_trace(ch, "ssh_error", s->identity.mac, s->peer,
                         "chssh_channel_send failed");
                return -1;
            }
        }
    }
    if (session_drain_chssh(ch, s) != 0) {
        return -1;
    }
    return 0;
}

/**
 * Calix CMS-compatible SSH (from /tmp/callback.pcap):
 *   after identity NMS already sent "<ack>ok</ack>"; E7 is SSH *server*;
 *   NMS is SSH *client* → subsystem netconf → NETCONF client.
 */
static int session_start_chssh(edge_e7_callhome_t *ch, edge_e7_session_t *s)
{
    chssh_config_t ccfg;
    const char *user = "sysadmin";
    const char *pass = "sysadmin";

    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.lab_mode = 0;
    ccfg.hold_ident = 0;
    ccfg.accept_any_hostkey = 1;
    if (ch->cfg->e7_ssh_username[0]) {
        user = ch->cfg->e7_ssh_username;
    }
    if (ch->cfg->e7_ssh_password[0]) {
        pass = ch->cfg->e7_ssh_password;
    }
    ccfg.client_username = user;
    ccfg.client_password = pass;

    s->chssh = chssh_create(CHSSH_ROLE_CLIENT, &ccfg);
    if (!s->chssh) {
        e7_trace(ch, "chssh_create_fail", s->identity.mac, s->peer,
                 "chssh_create(CLIENT) returned NULL");
        return -1;
    }
    s->use_ssh = 1;
    s->use_chssh = 1;
    s->chssh_ready = 0;
    s->hello_sent = 0;
    s->last_nc_state = -1;
    s->nc_as_server = 0;
    s->state = EDGE_E7_SESS_SSH;
    s->ssh_banner_flushed = chssh_ident_flushed(s->chssh);
    s->saw_ssh_rx = 0;
    s->identity_ms = mono_now_ms();

    e7_trace(ch, "ssh_client", s->identity.mac, s->peer,
             "libchssh SSH client after calix ack (E7 is SSH server); "
             "user=%s subsystem=netconf",
             user);

    if (s->id_len > 0) {
        size_t used = chssh_feed_input(s->chssh, (const uint8_t *)s->id_buf,
                                       s->id_len);
        e7_trace(ch, "ssh_rx", s->identity.mac, s->peer,
                 "fed %zu leftover post-identity bytes into chssh client",
                 used);
        s->id_len = 0;
        s->saw_ssh_rx = 1;
    }

    if (session_chssh_process(ch, s) != 0) {
        session_clear_nc(s);
        return -1;
    }
    if (session_flush_tx(ch, s) != 0) {
        e7_trace(ch, "write_err", s->identity.mac, s->peer,
                 "flush SSH client output errno=%d", errno);
        session_clear_nc(s);
        return -1;
    }
    return 0;
}

/** Calix identity-layer ACK from working CMS capture: exactly "<ack>ok</ack>". */
static int session_send_calix_ack_ok(edge_e7_callhome_t *ch, edge_e7_session_t *s)
{
    static const char ack[] = "<ack>ok</ack>";
    return session_send_raw(ch, s, ack, sizeof(ack) - 1, "calix <ack>ok</ack>");
}
#endif /* EDGEHOST_E7_CHSSH_AVAILABLE */

/**
 * Start libnetconf SM after identity on the *same* Call Home TCP fd.
 * @p force_ssh 1 = SSH after identity (RFC 8071 roles), 0 = raw NETCONF.
 * @p as_server raw-probe only: NETCONF server-shaped hello.
 *
 * SSH prefers libchssh (production KEX). Fallback: libnetconf+libassh.
 * NETCONF app role stays CLIENT after subsystem open.
 */
static int session_start_netconf(edge_e7_callhome_t *ch, edge_e7_session_t *s,
                                 int force_ssh, int as_server)
{
    netconf_config_t ncfg;
    int use_ssh = force_ssh ? 1 : 0;
    netconf_role_t role;
    const char *ssh_user = "sysadmin";

    if (!ch || !ch->cfg || !s) {
        return -1;
    }

    if (use_ssh) {
#if EDGEHOST_E7_CHSSH_AVAILABLE
        return session_start_chssh(ch, s);
#elif EDGEHOST_E7_SSH_AVAILABLE
        /* legacy libassh-in-libnetconf path below */
#else
        e7_trace(ch, "ssh_unavailable",
                 s->identity.identity_ok ? s->identity.mac : "", s->peer,
                 "EDGEHOST_E7_SSH_AVAILABLE=0 (build libchssh or libassh)");
        return -1;
#endif
    }

    edge_e7_netconf_profile(&ncfg);
    s->use_ssh = 0;
    s->use_chssh = 0;
    s->hello_sent = 0;
    s->last_nc_state = -1;
    s->nc_as_server = as_server ? 1 : 0;
    role = s->nc_as_server ? NETCONF_ROLE_CALLHOME_SERVER : NETCONF_ROLE_CLIENT;

#if EDGEHOST_E7_SSH_AVAILABLE && !EDGEHOST_E7_CHSSH_AVAILABLE
    if (use_ssh) {
        edge_e7_netconf_apply_ssh(&ncfg, ch->cfg);
        if (ch->cfg->e7_ssh_username[0]) {
            ssh_user = ch->cfg->e7_ssh_username;
        }
        s->use_ssh = 1;
    }
#else
    (void)ssh_user;
#endif

    s->nc = netconf_create_with_config(role, &ncfg);
    if (!s->nc) {
        e7_trace(ch, "netconf_create_fail",
                 s->identity.identity_ok ? s->identity.mac : "", s->peer,
                 "netconf_create_with_config returned NULL (nc_role=%s ssh=%d)",
                 s->nc_as_server ? "server" : "client", use_ssh);
        return -1;
    }
    e7_trace(ch, "nc_role", s->identity.identity_ok ? s->identity.mac : "",
             s->peer, "NETCONF_ROLE=%s ssh_mode=%s",
             s->nc_as_server ? "CALLHOME_SERVER" : "CLIENT",
             use_ssh ? "SSH_CALLHOME(server)" : "OFF(raw)");

    drain_nc_events(ch, s);
    if (session_drain_output(s) != 0) {
        session_clear_nc(s);
        e7_trace(ch, "netconf_start_fail",
                 s->identity.identity_ok ? s->identity.mac : "", s->peer,
                 "initial output drain failed");
        return -1;
    }

    s->identity_ms = mono_now_ms();
#if EDGEHOST_E7_SSH_AVAILABLE && !EDGEHOST_E7_CHSSH_AVAILABLE
    if (use_ssh) {
        s->state = EDGE_E7_SESS_SSH;
        s->ssh_banner_flushed = 0;
        s->saw_ssh_rx = 0;
        e7_trace(ch, "ssh_server", s->identity.identity_ok ? s->identity.mac : "",
                 s->peer,
                 "legacy libassh SSH server after identity; user=%s",
                 ssh_user);
        session_trace_nc_state(ch, s);
        if (s->tx_len > s->tx_off && !s->first_tx_logged) {
            s->first_tx_logged = 1;
            e7_trace_bytes(ch, "first_tx", s->identity.mac, s->peer,
                           "SSH server ident", s->tx + s->tx_off,
                           s->tx_len - s->tx_off);
        }
        if (session_flush_tx(ch, s) != 0) {
            session_clear_nc(s);
            return -1;
        }
        s->ssh_banner_flushed = 1;
        e7_trace(ch, "ssh_banner_sent", s->identity.mac, s->peer,
                 "SSH server identification flushed (libassh)");
        return 0;
    }
#endif

    if (session_try_send_hello(ch, s) != 0) {
        session_clear_nc(s);
        e7_trace(ch, "hello_fail",
                 s->identity.identity_ok ? s->identity.mac : "", s->peer,
                 "initial hello failed");
        return -1;
    }
    return 0;
}

static void session_trace_nc_state(edge_e7_callhome_t *ch, edge_e7_session_t *s)
{
    netconf_state_t st;
    if (!ch || !s || !s->nc) {
        return;
    }
    st = netconf_current_state(s->nc);
    if ((int)st == s->last_nc_state) {
        return;
    }
    s->last_nc_state = (int)st;
    e7_trace(ch, "nc_state", s->identity.identity_ok ? s->identity.mac : "",
             s->peer, "%s (sess=%s)", e7_nc_state_name(st),
             edge_e7_sess_state_name(s->state));
}

static void session_check_open(edge_e7_callhome_t *ch, edge_e7_session_t *s)
{
    if (!s->nc) {
        return;
    }
    session_trace_nc_state(ch, s);
    if (netconf_current_state(s->nc) == NETCONF_STATE_SESSION_OPEN &&
        s->state != EDGE_E7_SESS_OPEN) {
        s->state = EDGE_E7_SESS_OPEN;
        s->open_ms = mono_now_ms();
        s->last_activity_ms = s->open_ms;
        s->last_keepalive_ms = s->open_ms;
        ch->stats.sessions_open++;
        ch->stats.sessions_opened++;
        put_session_json(ch, s, "open");
        e7_trace(ch, "open", s->identity.identity_ok ? s->identity.mac : "",
                 s->peer, "NETCONF SESSION_OPEN (probe=%s)",
                 e7_probe_mode_name(s->probe_mode));
        {
            edge_e7_runtime_shelf_t *rs =
                runtime_find(ch, s->identity.mac);
            if (rs) {
                /* Lock successful strategy for future dials. */
                rs->probe_mode = s->probe_mode;
                e7_trace(ch, "probe_success", s->identity.mac, s->peer,
                         "locking mode %u (%s)", (unsigned)s->probe_mode,
                         e7_probe_mode_name(s->probe_mode));
            }
        }
        session_try_subscribe(ch, s);
    }
}

/** Post feed/drain: maybe finish SSH → hello, then check OPEN. */
static void session_after_nc_io(edge_e7_callhome_t *ch, edge_e7_session_t *s)
{
    if (!s || !s->nc) {
        return;
    }
    session_trace_nc_state(ch, s);
    if (!s->hello_sent) {
        if (session_try_send_hello(ch, s) != 0) {
            ch->stats.rejects_other++;
            put_session_json(ch, s, "error");
            e7_trace(ch, "hello_fail",
                     s->identity.identity_ok ? s->identity.mac : "", s->peer,
                     "send hello after SSH failed");
            session_close(ch, s);
            return;
        }
    }
    if (s->state == EDGE_E7_SESS_EMPTY) {
        return;
    }
    if (session_drain_output(s) != 0) {
        e7_trace(ch, "io_error",
                 s->identity.identity_ok ? s->identity.mac : "", s->peer,
                 "tx drain failed");
        session_close(ch, s);
        return;
    }
    session_check_open(ch, s);
    if (s->state == EDGE_E7_SESS_OPEN && !s->sub_sent) {
        session_try_subscribe(ch, s);
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
        e7_trace(ch, "identity_bad", "", s->peer,
                 "parse failed or incomplete identity (%zu bytes)", id_end);
        session_close(ch, s);
        return -1;
    }
    s->identity = id;
    e7_trace(ch, "identity_ok", s->identity.mac, s->peer,
             "serial=%s model=%s", s->identity.serial, s->identity.model);

    look = shelf_lookup_runtime(ch, s->identity.mac);
    if (look == 0) {
        /* present but disabled */
        put_session_json(ch, s, "disabled");
        ch->stats.rejects_disabled++;
        e7_trace(ch, "reject_disabled", s->identity.mac, s->peer,
                 "shelf on allowlist but enabled=false");
        session_close(ch, s);
        return -1;
    }
    if (look < 0) {
        if (!ch->cfg->e7_auto_subscribe_unknown) {
            put_session_json(ch, s, "unconfigured");
            ch->stats.rejects_not_allowlisted++;
            e7_trace(ch, "reject_unconfigured", s->identity.mac, s->peer,
                     "MAC not on allowlist — add shelf on /e7/ then retry");
            session_close(ch, s);
            return -1;
        }
        s->auto_unknown = 1;
        s->allowlisted = 0;
        e7_trace(ch, "auto_unknown", s->identity.mac, s->peer,
                 "MAC not allowlisted; auto_subscribe_unknown=true");
    } else {
        s->allowlisted = 1;
        s->auto_unknown = 0;
        e7_trace(ch, "allowlist_ok", s->identity.mac, s->peer,
                 "MAC matched runtime allowlist");
    }

    /* Hex-log full identity preamble (field wire forensics). */
    e7_trace_bytes(ch, "identity_hex", s->identity.mac, s->peer, "identity",
                   s->id_buf, id_end);

    /* Shift any trailing bytes after identity into a leftover buffer. */
    {
        size_t rem = s->id_len > id_end ? s->id_len - id_end : 0;
        size_t off = 0;
        if (rem > 0) {
            memmove(s->id_buf, s->id_buf + id_end, rem);
        }
        s->id_len = rem;
        /* Strip leading whitespace after </identity>. */
        while (off < s->id_len &&
               (s->id_buf[off] == ' ' || s->id_buf[off] == '\t' ||
                s->id_buf[off] == '\r' || s->id_buf[off] == '\n')) {
            off++;
        }
        if (off > 0) {
            if (off < s->id_len) {
                memmove(s->id_buf, s->id_buf + off, s->id_len - off);
                s->id_len -= off;
            } else {
                s->id_len = 0;
            }
        }
    }

#if EDGEHOST_HAVE_LIBNETCONF
    /*
     * Post-identity:
     *  - leftover SSH- from E7: start SSH server and feed immediately.
     *  - transport ssh: silent hold → identity ACK ladder → SSH server
     *    (field E7 rejects immediate wrong TX; capture peer-first if any).
     *  - transport raw: probe modes (unit tests / experimental cleartext).
     */
    {
        edge_e7_runtime_shelf_t *rs = runtime_find(ch, s->identity.mac);
        s->probe_mode = rs ? rs->probe_mode : EDGE_E7_PROBE_NC_CLIENT;
        s->probe_spoke = 0;
        s->probe_ack_sent = 0;
        s->ssh_phase = EDGE_E7_SSH_PHASE_HOLD;
        s->first_tx_logged = 0;
    }

    if (s->id_len > 0) {
        e7_trace_bytes(ch, "post_identity_leftover", s->identity.mac, s->peer,
                       "after </identity>", s->id_buf, s->id_len);
    }

    if (s->id_len >= 4 &&
        s->id_buf[0] == 'S' && s->id_buf[1] == 'S' && s->id_buf[2] == 'H' &&
        s->id_buf[3] == '-') {
        e7_trace(ch, "detect_ssh_client", s->identity.mac, s->peer,
                 "E7 SSH client banner in post-identity bytes — NMS SSH server");
        if (session_begin_after_identity(ch, s, 1) != 0) {
            return -1;
        }
        return 0;
    }

    /*
     * Primary Calix path (transport: ssh) — from successful CMS capture:
     *   1) send "<ack>ok</ack>" (no trailing NL)
     *   2) wait for E7 SSH *server* identification
     *   3) NMS acts as SSH *client* (libchssh CLIENT)
     */
    if (ch->cfg && e7_transport_is_ssh(ch->cfg->e7_transport)) {
        s->state = EDGE_E7_SESS_POST_ID;
        s->identity_ms = mono_now_ms();
        s->probe_mode = EDGE_E7_PROBE_SILENT;
        s->ssh_phase = EDGE_E7_SSH_PHASE_HOLD; /* wait for peer SSH after ack */
        s->ssh_resume = EDGE_E7_SSH_PHASE_SSH;

        e7_trace(ch, "post_identity", s->identity.mac, s->peer,
                 "identity complete — calix ack then wait for E7 SSH server "
                 "(NMS will be SSH client)");
#if EDGEHOST_E7_CHSSH_AVAILABLE
        if (session_send_calix_ack_ok(ch, s) != 0) {
            session_close(ch, s);
            return -1;
        }
        e7_trace(ch, "calix_ack", s->identity.mac, s->peer,
                 "sent <ack>ok</ack> — waiting for peer SSH- identification");
#else
        e7_trace(ch, "post_identity_hold", s->identity.mac, s->peer,
                 "libchssh unavailable; silent hold then legacy path");
#endif
        return 0;
    }

    if (s->id_len >= 1 && s->id_buf[0] == '<') {
        e7_trace(ch, "detect_raw_hello", s->identity.mac, s->peer,
                 "XML after identity — starting raw NETCONF");
        s->probe_mode = EDGE_E7_PROBE_NC_CLIENT;
        if (session_begin_after_identity(ch, s, 0) != 0) {
            return -1;
        }
        return 0;
    }

    /* auto: short wait, then SSH client if peer stays quiet */
    if (ch->cfg && e7_transport_is_auto(ch->cfg->e7_transport)) {
        s->state = EDGE_E7_SESS_POST_ID;
        s->identity_ms = mono_now_ms();
        e7_trace(ch, "post_identity_wait", s->identity.mac, s->peer,
                 "auto: wait %ums then SSH client if peer silent",
                 (unsigned)EDGE_E7_POST_ID_WAIT_MS);
        return 0;
    }

    /* transport: raw — experimental probe ladder (unit tests use client hello) */
    e7_trace(ch, "probe", s->identity.mac, s->peer, "mode %u (%s) for this dial",
             (unsigned)s->probe_mode, e7_probe_mode_name(s->probe_mode));

    if (s->probe_mode == EDGE_E7_PROBE_NC_CLIENT ||
        s->probe_mode == EDGE_E7_PROBE_NC_SERVER) {
        if (session_begin_after_identity(ch, s, 0) != 0) {
            return -1;
        }
        return 0;
    }

    s->state = EDGE_E7_SESS_POST_ID;
    s->identity_ms = mono_now_ms();
    if (s->probe_mode == EDGE_E7_PROBE_VERSION_ACK ||
        s->probe_mode == EDGE_E7_PROBE_VERSION_ACK2) {
        if (session_send_version_ack(
                ch, s, s->probe_mode == EDGE_E7_PROBE_VERSION_ACK2) != 0) {
            session_close(ch, s);
            return -1;
        }
        e7_trace(ch, "probe_wait", s->identity.mac, s->peer,
                 "waiting up to %ums for peer after version ACK",
                 (unsigned)EDGE_E7_PROBE_ACK_WAIT_MS);
    } else {
        e7_trace(ch, "probe_wait", s->identity.mac, s->peer,
                 "silent listen up to %ums (no NMS TX) — capturing peer-first data",
                 (unsigned)EDGE_E7_PROBE_SILENT_MS);
    }
    return 0;
#else
    (void)ch;
    session_close(ch, s);
    return -1;
#endif
}

#if EDGEHOST_HAVE_LIBNETCONF
/** Start netconf/SSH and feed any buffered post-identity bytes. */
static int session_begin_after_identity(edge_e7_callhome_t *ch,
                                        edge_e7_session_t *s, int use_ssh)
{
    int as_server = (s->probe_mode == EDGE_E7_PROBE_NC_SERVER) ? 1 : 0;

    if (session_start_netconf(ch, s, use_ssh, as_server) != 0) {
        ch->stats.rejects_other++;
        put_session_json(ch, s, "error");
        e7_trace(ch, "netconf_start_fail", s->identity.mac, s->peer,
                 "could not start %s after identity",
                 use_ssh ? "SSH" : "raw NETCONF");
        session_close(ch, s);
        return -1;
    }
    s->probe_spoke = 1;
    if (s->id_len > 0 && s->nc) {
        size_t used;
        e7_trace(ch, "post_identity_bytes", s->identity.mac, s->peer,
                 "%zu leftover bytes after </identity>", s->id_len);
        used = netconf_feed_input(s->nc, (const uint8_t *)s->id_buf, s->id_len);
        (void)used;
        s->id_len = 0;
        drain_nc_events(ch, s);
        session_after_nc_io(ch, s);
        if (s->state == EDGE_E7_SESS_EMPTY) {
            return -1;
        }
        if (session_flush_tx(ch, s) != 0) {
            e7_trace(ch, "write_err", s->identity.mac, s->peer,
                     "flush after leftover feed errno=%d", errno);
            session_close(ch, s);
            return -1;
        }
    }
    return 0;
}
#endif

/* ---- Call Home stream capture (host-visible after accept) ---- */

static void e7_pcap_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void e7_pcap_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t e7_ip_checksum(const uint8_t *hdr, size_t len)
{
    uint32_t sum = 0;
    size_t i;
    for (i = 0; i + 1 < len; i += 2) {
        sum += ((uint32_t)hdr[i] << 8) | hdr[i + 1];
    }
    if (i < len) {
        sum += (uint32_t)hdr[i] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static int e7_pcap_open(edge_e7_callhome_t *ch)
{
    static const uint8_t ghdr[24] = {
        0xd4, 0xc3, 0xb2, 0xa1, /* magic LE */
        0x02, 0x00, 0x04, 0x00, /* v2.4 */
        0x00, 0x00, 0x00, 0x00, /* thiszone */
        0x00, 0x00, 0x00, 0x00, /* sigfigs */
        0x00, 0x00, 0x04, 0x00, /* snaplen 262144 */
        0x65, 0x00, 0x00, 0x00  /* LINKTYPE_RAW = 101 */
    };
    const char *dir = "var";
    if (!ch || ch->pcap_fp) {
        return 0;
    }
    snprintf(ch->pcap_path, sizeof(ch->pcap_path),
             "%s/e7_callhome_capture.pcap", dir);
    snprintf(ch->pcap_text_path, sizeof(ch->pcap_text_path),
             "%s/e7_callhome_capture.log", dir);
    ch->pcap_fp = fopen(ch->pcap_path, "wb");
    ch->pcap_text_fp = fopen(ch->pcap_text_path, "a");
    if (!ch->pcap_fp) {
        fprintf(stderr, "edgehost: e7 pcap open failed path=%s errno=%d\n",
                ch->pcap_path, errno);
        if (ch->pcap_text_fp) {
            fclose(ch->pcap_text_fp);
            ch->pcap_text_fp = NULL;
        }
        return -1;
    }
    if (fwrite(ghdr, 1, sizeof(ghdr), ch->pcap_fp) != sizeof(ghdr)) {
        fclose(ch->pcap_fp);
        ch->pcap_fp = NULL;
        return -1;
    }
    fflush(ch->pcap_fp);
    ch->pcap_seq_c2s = 1000;
    ch->pcap_seq_s2c = 2000;
    ch->pcap_pkts = 0;
    ch->pcap_bytes = 0;
    fprintf(stderr,
            "edgehost: e7_callhome stream capture → %s (+ %s text)\n",
            ch->pcap_path, ch->pcap_text_path);
    if (ch->pcap_text_fp) {
        fprintf(ch->pcap_text_fp,
                "# e7_callhome host stream capture (post-accept app bytes)\n"
                "# Lines starting with \"# DIAL\" summarize each TCP dial:\n"
                "#   RX identity → hold → first TX → peer EOF\n");
        fflush(ch->pcap_text_fp);
    }
    return 0;
}

static void e7_pcap_close(edge_e7_callhome_t *ch)
{
    if (!ch) {
        return;
    }
    if (ch->pcap_fp) {
        fflush(ch->pcap_fp);
        fclose(ch->pcap_fp);
        ch->pcap_fp = NULL;
    }
    if (ch->pcap_text_fp) {
        fflush(ch->pcap_text_fp);
        fclose(ch->pcap_text_fp);
        ch->pcap_text_fp = NULL;
    }
}

/** Classify chunk and update per-dial counters for summary lines. */
static void e7_pcap_note_chunk(edge_e7_session_t *s, int to_peer,
                               const uint8_t *data, size_t len)
{
    uint64_t now;
    if (!s || !data || len == 0) {
        return;
    }
    now = mono_now_ms();
    if (s->cap_t0_ms == 0) {
        s->cap_t0_ms = now;
    }
    if (to_peer) {
        s->cap_tx_chunks++;
        s->cap_tx_bytes += (uint32_t)(len > 0xffffffffu ? 0xffffffffu : len);
        if (s->cap_first_tx_ms == 0) {
            s->cap_first_tx_ms = now;
            if (len >= 4 && data[0] == 'S' && data[1] == 'S' && data[2] == 'H' &&
                data[3] == '-') {
                s->cap_saw_ssh_tx = 1;
                snprintf(s->cap_first_tx_tag, sizeof(s->cap_first_tx_tag),
                         "SSH-banner");
            } else if (len >= 9 && data[0] == '<' &&
                       memcmp(data, "<version>", 9) == 0) {
                s->cap_saw_ack_tx = 1;
                snprintf(s->cap_first_tx_tag, sizeof(s->cap_first_tx_tag),
                         "version-ACK");
            } else {
                snprintf(s->cap_first_tx_tag, sizeof(s->cap_first_tx_tag),
                         "other");
            }
        } else if (len >= 4 && data[0] == 'S' && data[1] == 'S' &&
                   data[2] == 'H' && data[3] == '-') {
            s->cap_saw_ssh_tx = 1;
        } else if (len >= 9 && data[0] == '<' &&
                   memcmp(data, "<version>", 9) == 0) {
            s->cap_saw_ack_tx = 1;
        }
    } else {
        s->cap_rx_chunks++;
        s->cap_rx_bytes += (uint32_t)(len > 0xffffffffu ? 0xffffffffu : len);
        if (len >= 10 && memmem(data, len, "<identity>", 10) != NULL) {
            s->cap_saw_ident = 1;
            if (s->cap_ident_ms == 0) {
                s->cap_ident_ms = now;
            }
        }
        if (len >= 4 && data[0] == 'S' && data[1] == 'S' && data[2] == 'H' &&
            data[3] == '-') {
            s->cap_saw_ssh_rx = 1;
        }
    }
}

/**
 * One-line dial summary for the text capture log (and SPA-adjacent forensics).
 * Example:
 *   # DIAL #3 end peer=192.168.35.11 mac=00:02:5d:… rx=182B identity
 *     hold_ms=3000 first_tx=SSH-banner 21B eof_ms_after_tx=12 saw_peer_ssh=0
 */
static void e7_pcap_dial_summary(edge_e7_callhome_t *ch, edge_e7_session_t *s,
                                 const char *why)
{
    uint64_t now;
    uint64_t hold_ms = 0;
    uint64_t eof_after_tx = 0;
    const char *mac;
    const char *peer;
    struct timeval tv;

    if (!ch || !s || s->cap_summary_done) {
        return;
    }
    if (s->cap_rx_chunks == 0 && s->cap_tx_chunks == 0) {
        return; /* empty accept with no app data */
    }
    s->cap_summary_done = 1;
    now = mono_now_ms();
    if (s->cap_ident_ms && s->cap_first_tx_ms &&
        s->cap_first_tx_ms >= s->cap_ident_ms) {
        hold_ms = s->cap_first_tx_ms - s->cap_ident_ms;
    } else if (s->identity_ms && s->cap_first_tx_ms &&
               s->cap_first_tx_ms >= s->identity_ms) {
        hold_ms = s->cap_first_tx_ms - s->identity_ms;
    }
    if (s->cap_first_tx_ms && now >= s->cap_first_tx_ms) {
        eof_after_tx = now - s->cap_first_tx_ms;
    }
    mac = (s->identity.identity_ok && s->identity.mac[0]) ? s->identity.mac
                                                          : "-";
    peer = s->peer[0] ? s->peer : "-";
    gettimeofday(&tv, NULL);
    ch->pcap_dials++;

    if (ch->pcap_text_fp) {
        fprintf(ch->pcap_text_fp,
                "# DIAL #%llu end peer=%s mac=%s why=%s "
                "rx=%uB/%u chunk%s tx=%uB/%u chunk%s "
                "identity=%u hold_ms=%llu first_tx=%s "
                "ssh_tx=%u ssh_rx=%u ack_tx=%u "
                "eof_ms_after_first_tx=%llu state=%s\n",
                (unsigned long long)ch->pcap_dials, peer, mac,
                why ? why : "close", (unsigned)s->cap_rx_bytes,
                (unsigned)s->cap_rx_chunks,
                s->cap_rx_chunks == 1 ? "" : "s", (unsigned)s->cap_tx_bytes,
                (unsigned)s->cap_tx_chunks,
                s->cap_tx_chunks == 1 ? "" : "s",
                (unsigned)s->cap_saw_ident, (unsigned long long)hold_ms,
                s->cap_first_tx_tag[0] ? s->cap_first_tx_tag : "none",
                (unsigned)s->cap_saw_ssh_tx, (unsigned)s->cap_saw_ssh_rx,
                (unsigned)s->cap_saw_ack_tx,
                (unsigned long long)eof_after_tx,
                edge_e7_sess_state_name(s->state));
        fflush(ch->pcap_text_fp);
    }

    /* Also surface a short line in SPA events. */
    e7_trace(ch, "dial_summary", mac, peer,
             "#%llu rx=%uB tx=%uB ident=%u hold_ms=%llu first_tx=%s "
             "ssh_tx=%u ssh_rx=%u eof_ms=%llu why=%s",
             (unsigned long long)ch->pcap_dials, (unsigned)s->cap_rx_bytes,
             (unsigned)s->cap_tx_bytes, (unsigned)s->cap_saw_ident,
             (unsigned long long)hold_ms,
             s->cap_first_tx_tag[0] ? s->cap_first_tx_tag : "none",
             (unsigned)s->cap_saw_ssh_tx, (unsigned)s->cap_saw_ssh_rx,
             (unsigned long long)eof_after_tx, why ? why : "close");
}

/**
 * Record one host-visible RX/TX chunk as a synthetic IPv4/TCP packet (RAW IP
 * PCAP). Captures exactly what e7_callhome read()/write() on the accepted fd
 * after io_uring E7 accept — not AF_PACKET L2 frames.
 *
 * @p to_peer 1 = NMS→E7 (write), 0 = E7→NMS (read)
 */
static void e7_pcap_write(edge_e7_callhome_t *ch, edge_e7_session_t *s,
                          int to_peer, const uint8_t *data, size_t len)
{
    struct sockaddr_storage lss, pss;
    socklen_t ll = sizeof(lss), pl = sizeof(pss);
    uint32_t sip = 0, dip = 0;
    uint16_t sport = 0, dport = 0;
    uint8_t *frame = NULL;
    size_t frame_len;
    uint8_t *ip;
    uint8_t *tcp;
    uint32_t seq;
    struct timeval tv;
    uint8_t rec_hdr[16];
    size_t i;

    if (!ch || !data || len == 0) {
        return;
    }
    if (!ch->pcap_fp && e7_pcap_open(ch) != 0) {
        return;
    }
    if (!ch->pcap_fp) {
        return;
    }
    if (s) {
        e7_pcap_note_chunk(s, to_peer, data, len);
    }

    memset(&lss, 0, sizeof(lss));
    memset(&pss, 0, sizeof(pss));
    if (s && s->fd >= 0) {
        (void)getsockname(s->fd, (struct sockaddr *)&lss, &ll);
        (void)getpeername(s->fd, (struct sockaddr *)&pss, &pl);
    }
    if (lss.ss_family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)&lss;
        sip = ntohl(a->sin_addr.s_addr);
        sport = ntohs(a->sin_port);
    }
    if (pss.ss_family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)&pss;
        dip = ntohl(a->sin_addr.s_addr);
        dport = ntohs(a->sin_port);
    }
    if (!to_peer) {
        uint32_t t = sip;
        uint16_t tp = sport;
        sip = dip;
        dip = t;
        sport = dport;
        dport = tp;
    }

    frame_len = 20 + 20 + len; /* IPv4 + TCP + payload */
    if (frame_len > 65535) {
        len = 65535 - 40;
        frame_len = 20 + 20 + len;
    }
    frame = (uint8_t *)malloc(frame_len);
    if (!frame) {
        return;
    }
    memset(frame, 0, frame_len);
    ip = frame;
    tcp = frame + 20;
    ip[0] = 0x45;
    e7_pcap_u16(ip + 2, (uint16_t)frame_len);
    e7_pcap_u16(ip + 4, (uint16_t)(ch->pcap_pkts + 1));
    ip[8] = 64;
    ip[9] = 6; /* TCP */
    e7_pcap_u32(ip + 12, sip);
    e7_pcap_u32(ip + 16, dip);
    e7_pcap_u16(ip + 10, e7_ip_checksum(ip, 20));

    e7_pcap_u16(tcp + 0, sport);
    e7_pcap_u16(tcp + 2, dport);
    if (to_peer) {
        seq = ch->pcap_seq_c2s;
        ch->pcap_seq_c2s += (uint32_t)len;
    } else {
        seq = ch->pcap_seq_s2c;
        ch->pcap_seq_s2c += (uint32_t)len;
    }
    e7_pcap_u32(tcp + 4, seq);
    e7_pcap_u32(tcp + 8, 0);
    tcp[12] = 0x50; /* data offset 5 */
    tcp[13] = 0x18; /* PSH+ACK */
    e7_pcap_u16(tcp + 14, 65535);
    memcpy(tcp + 20, data, len);

    gettimeofday(&tv, NULL);
    /* PCAP record header little-endian */
    rec_hdr[0] = (uint8_t)(tv.tv_sec);
    rec_hdr[1] = (uint8_t)(tv.tv_sec >> 8);
    rec_hdr[2] = (uint8_t)(tv.tv_sec >> 16);
    rec_hdr[3] = (uint8_t)(tv.tv_sec >> 24);
    rec_hdr[4] = (uint8_t)(tv.tv_usec);
    rec_hdr[5] = (uint8_t)(tv.tv_usec >> 8);
    rec_hdr[6] = (uint8_t)(tv.tv_usec >> 16);
    rec_hdr[7] = (uint8_t)(tv.tv_usec >> 24);
    rec_hdr[8] = (uint8_t)frame_len;
    rec_hdr[9] = (uint8_t)(frame_len >> 8);
    rec_hdr[10] = (uint8_t)(frame_len >> 16);
    rec_hdr[11] = (uint8_t)(frame_len >> 24);
    memcpy(rec_hdr + 12, rec_hdr + 8, 4);
    (void)fwrite(rec_hdr, 1, 16, ch->pcap_fp);
    (void)fwrite(frame, 1, frame_len, ch->pcap_fp);
    fflush(ch->pcap_fp);
    ch->pcap_pkts++;
    ch->pcap_bytes += len;

    if (ch->pcap_text_fp) {
        fprintf(ch->pcap_text_fp, "%ld.%06ld %s %s %zuB ",
                (long)tv.tv_sec, (long)tv.tv_usec,
                to_peer ? "TX(nms→e7)" : "RX(e7→nms)",
                s && s->identity.identity_ok ? s->identity.mac : "-",
                len);
        for (i = 0; i < len && i < 64; i++) {
            fprintf(ch->pcap_text_fp, "%02x", data[i]);
        }
        if (len > 64) {
            fprintf(ch->pcap_text_fp, "…");
        }
        fprintf(ch->pcap_text_fp, " | ");
        for (i = 0; i < len && i < 48; i++) {
            unsigned char c = data[i];
            fputc((c >= 32 && c < 127) ? (int)c : '.', ch->pcap_text_fp);
        }
        fputc('\n', ch->pcap_text_fp);
        fflush(ch->pcap_text_fp);
    }
    free(frame);
}

/* ---- I/O pump ---- */

static int session_flush_tx(edge_e7_callhome_t *ch, edge_e7_session_t *s)
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
        if (ch && n > 0) {
            e7_pcap_write(ch, s, 1, s->tx + s->tx_off, (size_t)n);
        }
        s->tx_off += (size_t)n;
    }
    s->tx_len = s->tx_off = 0;
    return 0;
}

/**
 * Process one readable chunk already in s->rx[0..n).
 * @return 0 continue, -1 session closed.
 */
static int session_handle_rx(edge_e7_callhome_t *ch, edge_e7_session_t *s,
                             size_t n)
{
    if (s->state == EDGE_E7_SESS_ACCEPTED || s->state == EDGE_E7_SESS_IDENTITY) {
        if (s->state == EDGE_E7_SESS_ACCEPTED) {
            e7_trace(ch, "identity", "", s->peer,
                     "receiving Calix identity preamble");
        }
        s->state = EDGE_E7_SESS_IDENTITY;
        if (s->id_len + n > EDGE_E7_IDENTITY_BUF_MAX) {
            ch->stats.rejects_bad_identity++;
            e7_trace(ch, "identity_overflow", "", s->peer,
                     "identity buffer > %u bytes",
                     (unsigned)EDGE_E7_IDENTITY_BUF_MAX);
            session_close(ch, s);
            return -1;
        }
        memcpy(s->id_buf + s->id_len, s->rx, n);
        s->id_len += n;
        {
            int end = find_identity_end(s->id_buf, s->id_len);
            if (end >= 0) {
                if (session_finish_identity(ch, s, (size_t)end) != 0) {
                    return -1;
                }
                if (session_flush_tx(ch, s) != 0) {
                    e7_trace(ch, "write_err",
                             s->identity.identity_ok ? s->identity.mac : "",
                             s->peer, "flush after identity errno=%d", errno);
                    session_close(ch, s);
                    return -1;
                }
            }
        }
        return 0;
    }

#if EDGEHOST_HAVE_LIBNETCONF
    /* After identity: peer may speak first (SSH client banner or raw hello). */
    if (s->state == EDGE_E7_SESS_POST_ID) {
        int use_ssh = 1;

        e7_trace_bytes(ch, "post_identity_rx", s->identity.mac, s->peer,
                       "peer", s->rx, n);

        if (n >= 1 && s->rx[0] == '<') {
            if (ch->cfg && e7_transport_is_ssh(ch->cfg->e7_transport)) {
                /*
                 * Peer XML during hold/ACK may be identity-layer reply or
                 * raw NETCONF. Prefer SSH path still; feed leftover after
                 * start only if SSH. Log and continue ladder only if we
                 * already spoke (ACK) — otherwise start SSH server? No:
                 * XML first from E7 after identity is unexpected for SSH
                 * Call Home; capture and still try SSH server after buffering
                 * is wrong. Buffer and escalate to SSH only if banner-like.
                 * For pure XML: if transport=ssh treat as unexpected and
                 * still attempt SSH server (prior raw hello path).
                 */
                e7_trace(ch, "unexpected_xml", s->identity.mac, s->peer,
                         "XML after identity (phase=%u spoke=%u) — "
                         "starting SSH server and feeding if leftover SSH only",
                         (unsigned)s->ssh_phase, (unsigned)s->probe_spoke);
                /* Buffer; do not feed XML into SSH. Stay in POST_ID if we
                 * have not finished hold — actually peer spoke so end hold. */
                if (s->id_len + n > EDGE_E7_IDENTITY_BUF_MAX) {
                    e7_trace(ch, "post_identity_overflow", s->identity.mac,
                             s->peer, "buffer full");
                    session_close(ch, s);
                    return -1;
                }
                memcpy(s->id_buf + s->id_len, s->rx, n);
                s->id_len += n;
                /* Do not start SSH with XML buffered into SSH feed — clear
                 * leftover so SSH starts clean; XML already hex-logged. */
                s->id_len = 0;
                if (session_begin_after_identity(ch, s, 1) != 0) {
                    return -1;
                }
                return 0;
            }
            if (ch->cfg && e7_transport_is_auto(ch->cfg->e7_transport)) {
                use_ssh = 0;
                e7_trace(ch, "detect_raw_hello", s->identity.mac, s->peer,
                         "XML after identity — raw NETCONF (auto)");
            } else {
                e7_trace(ch, "unexpected_xml", s->identity.mac, s->peer,
                         "XML after identity — still trying SSH");
            }
        } else if (n >= 4 && s->rx[0] == 'S' && s->rx[1] == 'S' &&
                   s->rx[2] == 'H' && s->rx[3] == '-') {
            e7_trace(ch, "detect_ssh_server", s->identity.mac, s->peer,
                     "peer SSH identification — E7 is SSH server; "
                     "NMS will be SSH client (Calix CMS roles)");
            use_ssh = 1;
        } else if (n >= 1 && s->rx[0] == 0x16) {
            e7_trace(ch, "detect_tls", s->identity.mac, s->peer,
                     "TLS record (0x16) — TLS Call Home not supported");
            session_close(ch, s);
            return -1;
        }

        /* Buffer peer bytes then start transport and feed. */
        if (s->id_len + n > EDGE_E7_IDENTITY_BUF_MAX) {
            e7_trace(ch, "post_identity_overflow", s->identity.mac, s->peer,
                     "buffer full");
            session_close(ch, s);
            return -1;
        }
        memcpy(s->id_buf + s->id_len, s->rx, n);
        s->id_len += n;
        if (session_begin_after_identity(ch, s, use_ssh) != 0) {
            return -1;
        }
        return 0;
    }
#endif

#if EDGEHOST_HAVE_LIBNETCONF || EDGEHOST_E7_CHSSH_AVAILABLE
    /* SSH (post-identity) or NETCONF hello/open. */
    if (s->state == EDGE_E7_SESS_SSH || s->state == EDGE_E7_SESS_HELLO ||
        s->state == EDGE_E7_SESS_OPEN) {
#if EDGEHOST_E7_CHSSH_AVAILABLE
        if (s->use_chssh && s->chssh) {
            size_t used;
            if (s->state == EDGE_E7_SESS_SSH && n > 0) {
                s->saw_ssh_rx = 1;
                e7_trace_bytes(ch, "ssh_rx",
                               s->identity.identity_ok ? s->identity.mac : "",
                               s->peer, "ssh", s->rx, n);
            }
            used = chssh_feed_input(s->chssh, s->rx, n);
            (void)used;
            if (session_chssh_process(ch, s) != 0) {
                session_close(ch, s);
                return -1;
            }
            if (session_flush_tx(ch, s) != 0) {
                e7_trace(ch, "write_err",
                         s->identity.identity_ok ? s->identity.mac : "",
                         s->peer, "flush after chssh feed errno=%d", errno);
                session_close(ch, s);
                return -1;
            }
            return 0;
        }
#endif
#if EDGEHOST_HAVE_LIBNETCONF
        if (s->nc) {
            size_t used;
            if (s->state == EDGE_E7_SESS_SSH && n > 0) {
                s->saw_ssh_rx = 1;
                e7_trace_bytes(ch, "ssh_rx",
                               s->identity.identity_ok ? s->identity.mac : "",
                               s->peer, "ssh", s->rx, n);
            }
            used = netconf_feed_input(s->nc, s->rx, n);
            (void)used;
            drain_nc_events(ch, s);
            session_after_nc_io(ch, s);
            if (s->state == EDGE_E7_SESS_EMPTY) {
                return -1;
            }
            if (session_flush_tx(ch, s) != 0) {
                e7_trace(ch, "write_err",
                         s->identity.identity_ok ? s->identity.mac : "",
                         s->peer, "flush after ssh/nc feed errno=%d", errno);
                session_close(ch, s);
                return -1;
            }
            return 0;
        }
#endif
    }
#else
    (void)ch;
    (void)n;
#endif
    return 0;
}

static void session_pump_read(edge_e7_callhome_t *ch, edge_e7_session_t *s)
{
    int guard;

    if (s->fd < 0 || !s->rx) {
        return;
    }
    /*
     * Drain the socket aggressively. Tick-only I/O (~200 ms) is too slow for
     * SSH KEX if we process a single read and leave responses buffered.
     */
    for (guard = 0; guard < 64; guard++) {
        ssize_t n = read(s->fd, s->rx, EDGE_E7_RX_CAP);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return;
            }
            e7_trace(ch, "read_err",
                     s->identity.identity_ok ? s->identity.mac : "", s->peer,
                     "errno=%d state=%s", errno,
                     edge_e7_sess_state_name(s->state));
            session_close(ch, s);
            return;
        }
        if (n > 0) {
            e7_pcap_write(ch, s, 0, s->rx, (size_t)n);
            /* Any peer bytes reset the idle clock used for keepalives. */
            s->last_activity_ms = mono_now_ms();
        }
        if (n == 0) {
            if (s->state == EDGE_E7_SESS_HELLO) {
                e7_trace(ch, "peer_eof",
                         s->identity.identity_ok ? s->identity.mac : "",
                         s->peer,
                         "peer closed during hello (probe=%s spoke=%u)",
                         e7_probe_mode_name(s->probe_mode),
                         (unsigned)s->probe_spoke);
                probe_advance_on_fail(ch, s);
            } else if (s->state == EDGE_E7_SESS_POST_ID) {
                e7_trace(ch, "peer_eof",
                         s->identity.identity_ok ? s->identity.mac : "",
                         s->peer,
                         "peer closed during probe=%s phase=%u spoke=%u%s",
                         e7_probe_mode_name(s->probe_mode),
                         (unsigned)s->ssh_phase, (unsigned)s->probe_spoke,
                         s->probe_spoke
                             ? ""
                             : " — identity-only? (NMS sent nothing)");
                /* transport:ssh — skip the phase that just failed on next dial */
                if (ch->cfg && e7_transport_is_ssh(ch->cfg->e7_transport) &&
                    s->identity.identity_ok) {
                    edge_e7_runtime_shelf_t *rs =
                        runtime_find(ch, s->identity.mac);
                    if (rs) {
                        /*
                         * Identity ACK killed the dial → next redial: hold then
                         * SSH server (skip remaining ACKs). Hold-only EOF →
                         * still try ACK0 once on the next dial.
                         */
                        uint8_t adv = (s->ssh_phase >= EDGE_E7_SSH_PHASE_ACK0)
                                          ? (uint8_t)EDGE_E7_SSH_PHASE_SSH
                                          : (uint8_t)EDGE_E7_SSH_PHASE_ACK0;
                        rs->ssh_field_next = adv;
                        e7_trace(ch, "ssh_field_advance", s->identity.mac,
                                 s->peer,
                                 "next dial resume phase=%u after eof at "
                                 "phase=%u",
                                 (unsigned)adv, (unsigned)s->ssh_phase);
                    }
                } else {
                    probe_advance_on_fail(ch, s);
                }
            } else if (s->state == EDGE_E7_SESS_SSH) {
                e7_trace(ch, "peer_eof",
                         s->identity.identity_ok ? s->identity.mac : "",
                         s->peer,
                         "peer closed during same-socket SSH "
                         "(nms_is_server banner_sent=%d saw_peer_ssh=%d)",
                         s->ssh_banner_flushed, s->saw_ssh_rx);
                if (ch->cfg && e7_transport_is_ssh(ch->cfg->e7_transport) &&
                    s->identity.identity_ok) {
                    edge_e7_runtime_shelf_t *rs =
                        runtime_find(ch, s->identity.mac);
                    if (rs) {
                        /* Stay on delayed-SSH after hold for further redials. */
                        rs->ssh_field_next = EDGE_E7_SSH_PHASE_SSH;
                    }
                }
            } else {
                e7_trace(ch, "peer_eof",
                         s->identity.identity_ok ? s->identity.mac : "", s->peer,
                         "peer closed TCP during %s",
                         edge_e7_sess_state_name(s->state));
            }
            session_close(ch, s);
            return;
        }
        if (session_handle_rx(ch, s, (size_t)n) != 0) {
            return;
        }
        if (s->state == EDGE_E7_SESS_EMPTY) {
            return;
        }
    }
}

static void session_on_timeout(edge_e7_callhome_t *ch, edge_e7_session_t *s,
                               uint64_t mono_ms)
{
    if (s->state == EDGE_E7_SESS_ACCEPTED || s->state == EDGE_E7_SESS_IDENTITY) {
        if (mono_ms > s->accepted_ms &&
            mono_ms - s->accepted_ms > EDGE_E7_IDENTITY_TIMEOUT_MS) {
            ch->stats.rejects_bad_identity++;
            e7_trace(ch, "identity_timeout",
                     s->identity.identity_ok ? s->identity.mac : "", s->peer,
                     "no complete identity within %ums",
                     (unsigned)EDGE_E7_IDENTITY_TIMEOUT_MS);
            session_close(ch, s);
        }
    } else if (s->state == EDGE_E7_SESS_POST_ID) {
#if EDGEHOST_HAVE_LIBNETCONF
        uint64_t wait_ms = EDGE_E7_POST_ID_WAIT_MS;
        int is_ssh_transport =
            ch->cfg && e7_transport_is_ssh(ch->cfg->e7_transport);
        int is_auto =
            ch->cfg && e7_transport_is_auto(ch->cfg->e7_transport);

        if (is_ssh_transport) {
            /* After calix <ack>ok</ack>, wait for E7 SSH server banner. */
            wait_ms = 10000u;
        } else if (s->probe_mode == EDGE_E7_PROBE_SILENT) {
            wait_ms = EDGE_E7_PROBE_SILENT_MS;
        } else if (s->probe_mode == EDGE_E7_PROBE_VERSION_ACK ||
                   s->probe_mode == EDGE_E7_PROBE_VERSION_ACK2) {
            wait_ms = EDGE_E7_PROBE_ACK_WAIT_MS;
        }

        if (mono_ms > s->identity_ms && mono_ms - s->identity_ms >= wait_ms) {
            /* --- transport:ssh after calix ack: peer must speak SSH --- */
            if (is_ssh_transport) {
                e7_trace(ch, "post_identity_timeout", s->identity.mac, s->peer,
                         "no peer SSH banner within %ums after <ack>ok</ack>",
                         (unsigned)wait_ms);
                session_close(ch, s);
                return;
            }

            if (is_auto) {
                e7_trace(ch, "post_identity_timeout", s->identity.mac, s->peer,
                         "auto: no peer data — starting SSH client");
                if (session_begin_after_identity(ch, s, 1) != 0) {
                    return;
                }
                return;
            }
            /* Raw probe timeout: cascade on same dial when useful. */
            if (s->probe_mode == EDGE_E7_PROBE_SILENT) {
                /*
                 * E7 stayed silent while we stayed silent — it is waiting for
                 * NMS. Immediately try identity-layer ACK on this connection
                 * (do not burn a redial).
                 */
                e7_trace(ch, "probe_cascade", s->identity.mac, s->peer,
                         "silent: no peer bytes in %ums — sending version ACK "
                         "on same dial",
                         (unsigned)wait_ms);
                s->probe_mode = EDGE_E7_PROBE_VERSION_ACK;
                s->identity_ms = mono_ms;
                {
                    edge_e7_runtime_shelf_t *rs =
                        runtime_find(ch, s->identity.mac);
                    if (rs) {
                        rs->probe_mode = EDGE_E7_PROBE_VERSION_ACK;
                    }
                }
                if (session_send_version_ack(ch, s, 0) != 0) {
                    probe_advance_on_fail(ch, s);
                    session_close(ch, s);
                }
                return;
            }
            if (s->probe_mode == EDGE_E7_PROBE_VERSION_ACK ||
                s->probe_mode == EDGE_E7_PROBE_VERSION_ACK2) {
                e7_trace(ch, "probe_timeout", s->identity.mac, s->peer,
                         "no reply to version ACK in %ums — advancing",
                         (unsigned)wait_ms);
                probe_advance_on_fail(ch, s);
                session_close(ch, s);
                return;
            }
            e7_trace(ch, "post_identity_timeout", s->identity.mac, s->peer,
                     "starting raw NETCONF client hello");
            s->probe_mode = EDGE_E7_PROBE_NC_CLIENT;
            if (session_begin_after_identity(ch, s, 0) != 0) {
                return;
            }
        }
#else
        (void)mono_ms;
        session_close(ch, s);
#endif
    } else if (s->state == EDGE_E7_SESS_SSH || s->state == EDGE_E7_SESS_HELLO) {
        uint64_t base = s->identity_ms ? s->identity_ms : s->accepted_ms;
        if (mono_ms > base && mono_ms - base > EDGE_E7_HELLO_TIMEOUT_MS) {
            ch->stats.rejects_other++;
            put_session_json(ch, s,
                             s->state == EDGE_E7_SESS_SSH ? "ssh_timeout"
                                                          : "hello_timeout");
            e7_trace(ch,
                     s->state == EDGE_E7_SESS_SSH ? "ssh_timeout"
                                                  : "hello_timeout",
                     s->identity.identity_ok ? s->identity.mac : "", s->peer,
                     "no progress within %ums after identity (saw_ssh_rx=%d)",
                     (unsigned)EDGE_E7_HELLO_TIMEOUT_MS, s->saw_ssh_rx);
            session_close(ch, s);
        }
    }
}

#if EDGEHOST_E7_CHSSH_AVAILABLE
/**
 * Send SSH ClientAlive traffic every EDGE_E7_KEEPALIVE_MS of *our* TX silence.
 * Must NOT key off RX: E7 notifications do not reset OpenSSH ClientAlive.
 */
static void session_maybe_keepalive(edge_e7_callhome_t *ch, edge_e7_session_t *s,
                                    uint64_t mono_ms)
{
    uint64_t base;

    if (!s || !s->use_chssh || !s->chssh || !s->chssh_ready) {
        return;
    }
    if (s->state != EDGE_E7_SESS_OPEN && s->state != EDGE_E7_SESS_HELLO) {
        return;
    }
    base = s->last_keepalive_ms ? s->last_keepalive_ms : s->open_ms;
    if (base == 0) {
        s->last_keepalive_ms = mono_ms;
        return;
    }
    if (mono_ms < base + EDGE_E7_KEEPALIVE_MS) {
        return;
    }
    if (chssh_send_keepalive(s->chssh) != 0) {
        e7_trace(ch, "keepalive_fail", s->identity.mac, s->peer,
                 "chssh_send_keepalive failed");
        return;
    }
    if (session_drain_chssh(ch, s) != 0) {
        e7_trace(ch, "keepalive_fail", s->identity.mac, s->peer,
                 "drain after keepalive failed");
        return;
    }
    s->last_keepalive_ms = mono_ms;
    e7_trace(ch, "keepalive", s->identity.mac, s->peer,
             "SSH ClientAlive TX interval=%ums (rx does not count)",
             (unsigned)EDGE_E7_KEEPALIVE_MS);
}
#endif

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
    if (session_flush_tx(ch, s) != 0) {
        e7_trace(ch, "write_err",
                 s->identity.identity_ok ? s->identity.mac : "", s->peer,
                 "tx flush errno=%d state=%s", errno,
                 edge_e7_sess_state_name(s->state));
        session_close(ch, s);
        return;
    }
    session_pump_read(ch, s);
    if (s->state == EDGE_E7_SESS_EMPTY) {
        return;
    }
#if EDGEHOST_E7_CHSSH_AVAILABLE
    session_maybe_keepalive(ch, s, mono_ms);
    if (s->state == EDGE_E7_SESS_EMPTY) {
        return;
    }
#endif
    if (session_flush_tx(ch, s) != 0) {
        e7_trace(ch, "write_err",
                 s->identity.identity_ok ? s->identity.mac : "", s->peer,
                 "post-read tx flush errno=%d", errno);
        session_close(ch, s);
    }
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
    uint32_t dirty_cap;
    size_t est;

#if !EDGEHOST_HAVE_LIBNETCONF
    (void)opts;
    return NULL;
#else
    if (!opts || !opts->cfg || !opts->cfg->e7_enabled) {
        return NULL;
    }
    if (e7_transport_may_ssh(opts->cfg->e7_transport)) {
#if !EDGEHOST_E7_SSH_AVAILABLE
        fprintf(stderr,
                "edgehost: SSH Call Home requires libnetconf built with "
                "libassh (EDGEHOST_E7_SSH_AVAILABLE=0; install libassh "
                "under /usr/local and rebuild sibling libnetconf)\n");
        return NULL;
#endif
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

    dirty_cap = opts->cfg->e7_dirty_cap;
    if (dirty_cap == 0) {
        dirty_cap = EDGE_E7_DIRTY_CAP_DEFAULT;
    }

    ch = (edge_e7_callhome_t *)calloc(1, sizeof(*ch));
    if (!ch) {
        return NULL;
    }
    ch->cfg = opts->cfg;
    ch->state = opts->state;
    ch->hub = opts->hub;
    ch->listen_fd = -1;
    ch->bound_host[0] = '\0';
    ch->bound_port = 0;
    ch->bound_enabled = 0;
    ch->max_sessions = max_s;
    ch->dirty_cap = dirty_cap;
    ch->sessions =
        (edge_e7_session_t *)calloc(max_s, sizeof(edge_e7_session_t));
    if (!ch->sessions) {
        free(ch);
        return NULL;
    }
    ch->dirty = (edge_e7_dirty_slot_t *)calloc(dirty_cap,
                                               sizeof(edge_e7_dirty_slot_t));
    if (!ch->dirty) {
        free(ch->sessions);
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

    /* Host stream capture (post-accept app bytes) for field forensics. */
    (void)e7_pcap_open(ch);

#if EDGEHOST_HAVE_LIBYANG
    e7_yang_init(ch);
#endif

    /* Enable producer namespaces when store is available. */
    if (ch->state) {
        (void)edge_state_ns_set_enabled(ch->state, "inventory", 1);
        (void)edge_state_ns_set_enabled(ch->state, "net.pon", 1);
    }
    seed_runtime_from_yaml(ch);
    /* File wins for listed MACs over YAML when path configured (restart). */
    load_runtime_from_file(ch);
    return ch;
#endif
}

void edge_e7_callhome_destroy(edge_e7_callhome_t *ch)
{
    if (!ch) {
        return;
    }
    edge_e7_callhome_close_all(ch);
    if (ch->pcap_fp || ch->pcap_text_fp) {
        fprintf(stderr,
                "edgehost: e7_callhome capture closed pkts=%llu bytes=%llu "
                "file=%s\n",
                (unsigned long long)ch->pcap_pkts,
                (unsigned long long)ch->pcap_bytes, ch->pcap_path);
    }
    e7_pcap_close(ch);
#if EDGEHOST_HAVE_LIBYANG
    if (ch->yang) {
        yang_destroy(ch->yang);
        ch->yang = NULL;
    }
#endif
    free(ch->sessions);
    free(ch->dirty);
    free(ch);
}

int edge_e7_callhome_apply_config(edge_e7_callhome_t *ch,
                                  const edge_config_t *new_cfg)
{
    const char *policy;
    int replace_all = 0;
    const char *old_host;
    uint16_t old_port;
    int old_enabled;

    if (!ch || !new_cfg) {
        return -1;
    }

    old_host = ch->bound_host[0]
                   ? ch->bound_host
                   : (ch->cfg && ch->cfg->e7_listen_host[0]
                          ? ch->cfg->e7_listen_host
                          : "127.0.0.1");
    old_port = ch->bound_port
                   ? ch->bound_port
                   : (ch->cfg && ch->cfg->e7_listen_port
                          ? ch->cfg->e7_listen_port
                          : 4334u);
    old_enabled = ch->bound_enabled
                      ? 1
                      : (ch->cfg ? (ch->cfg->e7_enabled ? 1 : 0) : 0);

    if (strcmp(old_host, new_cfg->e7_listen_host[0] ? new_cfg->e7_listen_host
                                                    : "127.0.0.1") != 0 ||
        old_port != (new_cfg->e7_listen_port ? new_cfg->e7_listen_port
                                             : 4334u) ||
        old_enabled != (new_cfg->e7_enabled ? 1 : 0)) {
        fprintf(stderr,
                "edgehost: e7_callhome listen host/port/enabled changed on "
                "reload — restart required to rebind (allowlist still applied)\n");
    }

    ch->cfg = new_cfg;
    policy = new_cfg->e7_reload_policy[0] ? new_cfg->e7_reload_policy : "merge";
    if (strcmp(policy, "replace_all") == 0) {
        replace_all = 1;
    }

    if (replace_all) {
        runtime_clear_all(ch);
    }
    /* merge and replace_all: reseed/upsert YAML shelves (YAML MAC wins). */
    seed_runtime_from_yaml(ch);
    /* File path may have changed with new_cfg; re-merge durable shelves. */
    load_runtime_from_file(ch);
    save_runtime_to_file(ch);
    return 0;
}

void edge_e7_callhome_set_hub(edge_e7_callhome_t *ch, edge_ws_hub_t *hub)
{
    if (!ch) {
        return;
    }
    ch->hub = hub;
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

    if (e7_transport_may_ssh(ch->cfg->e7_transport)) {
#if !EDGEHOST_E7_SSH_AVAILABLE
        fprintf(stderr,
                "edgehost: e7_callhome bind: SSH Call Home requires "
                "libnetconf+libassh (EDGEHOST_E7_SSH_AVAILABLE=0)\n");
        return -1;
#endif
    }

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
    snprintf(ch->bound_host, sizeof(ch->bound_host), "%s", host);
    ch->bound_port = port;
    ch->bound_enabled = 1;
    fprintf(stderr, "edgehost: e7_callhome listening on %s:%u (%s)\n", host,
            (unsigned)port,
            e7_transport_is_ssh(ch->cfg->e7_transport)
                ? "ssh"
                : (e7_transport_is_auto(ch->cfg->e7_transport) ? "auto"
                                                               : "raw"));
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
        e7_trace(ch, "reject_other", "", "", "set_nonblock failed on accept");
        return -1;
    }
    s = session_alloc(ch);
    if (!s) {
        close(fd);
        ch->stats.rejects_capacity++;
        e7_trace(ch, "reject_capacity", "", "",
                 "session table full (max_sessions=%u)", ch->max_sessions);
        return -1;
    }
    s->fd = fd;
    s->state = EDGE_E7_SESS_ACCEPTED;
    s->accepted_ms = mono_now_ms();
    s->last_nc_state = -1;
    peer_to_str(peer, peer_len, s->peer, sizeof(s->peer));
    ch->stats.accepts++;
    e7_trace(ch, "accepted", "", s->peer,
             "TCP accept (transport=%s)",
             (ch->cfg && e7_transport_is_ssh(ch->cfg->e7_transport))
                 ? "ssh"
                 : ((ch->cfg && e7_transport_is_auto(ch->cfg->e7_transport))
                        ? "auto"
                        : "raw"));
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
    /* K16: flush dirty set on tick interval ≤100 ms. */
    if (ch->dirty_used > 0 &&
        (ch->last_flush_ms == 0 ||
         mono_ms - ch->last_flush_ms >= EDGE_E7_COALESCE_MS)) {
        dirty_flush(ch);
        ch->last_flush_ms = mono_ms;
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
    dirty_flush(ch);
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

/* ---- REST host APIs (PR-5) ---- */

static uint64_t stats_rejects_sum(const edge_e7_callhome_stats_t *st)
{
    if (!st) {
        return 0;
    }
    return st->rejects_bad_identity + st->rejects_not_allowlisted +
           st->rejects_disabled + st->rejects_capacity + st->rejects_other;
}

static uint64_t count_sessions_error(const edge_e7_callhome_t *ch)
{
    uint32_t i;
    uint64_t n = 0;
    if (!ch) {
        return 0;
    }
    for (i = 0; i < ch->max_sessions; i++) {
        if (ch->sessions[i].state == EDGE_E7_SESS_ERROR) {
            n++;
        }
    }
    return n;
}

int edge_e7_callhome_status_json(const edge_e7_callhome_t *ch, char *buf,
                                 size_t buf_sz)
{
    const edge_e7_callhome_stats_t *st;
    uint64_t drop_oldest = 0;
    uint64_t format_fail = 0;
    size_t rss_est;
    int n;

    if (!buf || buf_sz < 64) {
        return -1;
    }
    if (!ch) {
        n = snprintf(buf, buf_sz,
                     "{\"v\":1,\"enabled\":false,\"error\":\"E7_UNAVAILABLE\"}");
        return (n < 0 || (size_t)n >= buf_sz) ? -1 : n;
    }
    st = &ch->stats;
    if (ch->hub) {
        drop_oldest = edge_ws_hub_drop_oldest_count(ch->hub);
        format_fail = edge_ws_hub_format_fail_count(ch->hub);
    }
    rss_est = (size_t)ch->max_sessions * edge_e7_session_rss_estimate();
    n = snprintf(
        buf, buf_sz,
        "{"
        "\"v\":1,"
        "\"enabled\":%s,"
        "\"e7_accepts\":%llu,"
        "\"e7_sessions_open\":%llu,"
        "\"e7_sessions_error\":%llu,"
        "\"e7_notifications\":%llu,"
        "\"e7_state_puts\":%llu,"
        "\"e7_ws_fanouts\":%llu,"
        "\"e7_ws_coalesce_flush\":%llu,"
        "\"e7_ws_drop_oldest\":%llu,"
        "\"e7_ws_format_fail\":%llu,"
        "\"e7_coalesce_overflow\":%llu,"
        "\"e7_commands_ok\":%llu,"
        "\"e7_commands_err\":%llu,"
        "\"e7_rejects\":%llu,"
        "\"e7_unconfigured\":%llu,"
        "\"e7_rejects_bad_identity\":%llu,"
        "\"e7_rejects_disabled\":%llu,"
        "\"e7_rejects_capacity\":%llu,"
        "\"e7_rejects_other\":%llu,"
        "\"e7_sessions_opened\":%llu,"
        "\"e7_rss_estimate\":%zu,"
        "\"e7_subscriptions_ok\":%llu,"
        "\"max_sessions\":%u,"
        "\"runtime_shelves\":%u,"
        "\"transport\":\"%s\","
        "\"listen\":\"%s:%u\","
        "\"events_seq\":%llu"
        "}",
        edge_e7_callhome_enabled(ch) ? "true" : "false",
        (unsigned long long)st->accepts,
        (unsigned long long)st->sessions_open,
        (unsigned long long)count_sessions_error(ch),
        (unsigned long long)st->notifications,
        (unsigned long long)st->state_puts,
        (unsigned long long)st->ws_fanouts,
        (unsigned long long)st->coalesce_flush,
        (unsigned long long)drop_oldest,
        (unsigned long long)format_fail,
        (unsigned long long)st->coalesce_overflow,
        (unsigned long long)st->commands_ok,
        (unsigned long long)st->commands_err,
        (unsigned long long)stats_rejects_sum(st),
        (unsigned long long)st->rejects_not_allowlisted,
        (unsigned long long)st->rejects_bad_identity,
        (unsigned long long)st->rejects_disabled,
        (unsigned long long)st->rejects_capacity,
        (unsigned long long)st->rejects_other,
        (unsigned long long)st->sessions_opened,
        rss_est,
        (unsigned long long)st->subscriptions_ok,
        ch->max_sessions, ch->runtime_count,
        (ch->cfg && e7_transport_is_ssh(ch->cfg->e7_transport))
            ? "ssh"
            : ((ch->cfg && e7_transport_is_auto(ch->cfg->e7_transport)) ? "auto"
                                                                        : "raw"),
        ch->bound_host[0] ? ch->bound_host
                          : (ch->cfg && ch->cfg->e7_listen_host[0]
                                 ? ch->cfg->e7_listen_host
                                 : "0.0.0.0"),
        (unsigned)(ch->bound_port
                       ? ch->bound_port
                       : (ch->cfg && ch->cfg->e7_listen_port
                              ? ch->cfg->e7_listen_port
                              : 4334u)),
        (unsigned long long)ch->trace_seq);
    return (n < 0 || (size_t)n >= buf_sz) ? -1 : n;
}

int edge_e7_callhome_events_json(const edge_e7_callhome_t *ch, uint64_t since_id,
                                 char *buf, size_t buf_sz)
{
    size_t off = 0;
    uint32_t i;
    uint32_t n_live = 0;
    int first;
    int w;
    char esc_stage[EDGE_E7_TRACE_STAGE_MAX * 2 + 4];
    char esc_mac[EDGE_E7_MAC_MAX * 2 + 4];
    char esc_peer[EDGE_E7_PEER_ADDR_MAX * 2 + 4];
    char esc_detail[EDGE_E7_TRACE_DETAIL_MAX * 2 + 4];
    uint64_t now;
    const edge_e7_callhome_t *c = ch;

    if (!buf || buf_sz < 64) {
        return -1;
    }
    if (!c) {
        w = snprintf(buf, buf_sz,
                     "{\"v\":1,\"enabled\":false,\"error\":\"E7_UNAVAILABLE\","
                     "\"sessions\":[],\"events\":[]}");
        return (w < 0 || (size_t)w >= buf_sz) ? -1 : w;
    }
    now = mono_now_ms();

    w = snprintf(
        buf + off, buf_sz - off,
        "{\"v\":1,\"enabled\":%s,\"transport\":\"%s\",\"listen\":\"%s:%u\","
        "\"next_id\":%llu,\"since_id\":%llu,\"sessions\":[",
        edge_e7_callhome_enabled(c) ? "true" : "false",
        (c->cfg && e7_transport_is_ssh(c->cfg->e7_transport))
            ? "ssh"
            : ((c->cfg && e7_transport_is_auto(c->cfg->e7_transport)) ? "auto"
                                                                      : "raw"),
        c->bound_host[0]
            ? c->bound_host
            : (c->cfg && c->cfg->e7_listen_host[0] ? c->cfg->e7_listen_host
                                                   : "0.0.0.0"),
        (unsigned)(c->bound_port
                       ? c->bound_port
                       : (c->cfg && c->cfg->e7_listen_port
                              ? c->cfg->e7_listen_port
                              : 4334u)),
        (unsigned long long)c->trace_seq, (unsigned long long)since_id);
    if (w < 0 || (size_t)w >= buf_sz - off) {
        return -1;
    }
    off += (size_t)w;

    first = 1;
    for (i = 0; i < c->max_sessions; i++) {
        const edge_e7_session_t *s = &c->sessions[i];
        uint64_t age;
        const char *mac;
        if (s->state == EDGE_E7_SESS_EMPTY) {
            continue;
        }
        n_live++;
        age = (s->accepted_ms && now > s->accepted_ms) ? (now - s->accepted_ms)
                                                       : 0;
        mac = (s->identity.identity_ok && s->identity.mac[0]) ? s->identity.mac
                                                              : "";
        if (json_escape_str(mac, esc_mac, sizeof(esc_mac)) < 0 ||
            json_escape_str(s->peer, esc_peer, sizeof(esc_peer)) < 0) {
            return -1;
        }
        w = snprintf(buf + off, buf_sz - off,
                     "%s{\"slot\":%u,\"state\":\"%s\",\"mac\":%s,\"peer\":%s,"
                     "\"age_ms\":%llu,\"allowlisted\":%s,\"use_ssh\":%s}",
                     first ? "" : ",", (unsigned)i,
                     edge_e7_sess_state_name(s->state), esc_mac, esc_peer,
                     (unsigned long long)age, s->allowlisted ? "true" : "false",
                     s->use_ssh ? "true" : "false");
        if (w < 0 || (size_t)w >= buf_sz - off) {
            return -1;
        }
        off += (size_t)w;
        first = 0;
    }

    w = snprintf(buf + off, buf_sz - off,
                 "],\"live_sessions\":%u,\"events\":[", n_live);
    if (w < 0 || (size_t)w >= buf_sz - off) {
        return -1;
    }
    off += (size_t)w;

    /* Emit oldest→newest among retained ring entries with id > since_id. */
    first = 1;
    if (c->trace_count > 0) {
        uint32_t start =
            (c->trace_head + EDGE_E7_TRACE_CAP - c->trace_count) %
            EDGE_E7_TRACE_CAP;
        for (i = 0; i < c->trace_count; i++) {
            const edge_e7_trace_ev_t *ev =
                &c->trace[(start + i) % EDGE_E7_TRACE_CAP];
            if (ev->id <= since_id) {
                continue;
            }
            if (json_escape_str(ev->stage, esc_stage, sizeof(esc_stage)) < 0 ||
                json_escape_str(ev->mac, esc_mac, sizeof(esc_mac)) < 0 ||
                json_escape_str(ev->peer, esc_peer, sizeof(esc_peer)) < 0 ||
                json_escape_str(ev->detail, esc_detail, sizeof(esc_detail)) <
                    0) {
                return -1;
            }
            w = snprintf(buf + off, buf_sz - off,
                         "%s{\"id\":%llu,\"t_ms\":%llu,\"stage\":%s,\"mac\":%s,"
                         "\"peer\":%s,\"detail\":%s}",
                         first ? "" : ",", (unsigned long long)ev->id,
                         (unsigned long long)ev->t_ms, esc_stage, esc_mac,
                         esc_peer, esc_detail);
            if (w < 0 || (size_t)w >= buf_sz - off) {
                /* Truncate cleanly rather than fail the whole poll. */
                break;
            }
            off += (size_t)w;
            first = 0;
        }
    }

    if (off + 3 > buf_sz) {
        return -1;
    }
    buf[off++] = ']';
    buf[off++] = '}';
    buf[off] = '\0';
    return (int)off;
}

static int append_shelf_obj(const edge_e7_callhome_t *ch,
                            const edge_e7_runtime_shelf_t *rs, char *buf,
                            size_t buf_sz, size_t *off)
{
    edge_e7_sess_state_t st;
    const edge_e7_session_t *sess;
    char peer[EDGE_E7_PEER_ADDR_MAX];
    char serial[EDGE_E7_SERIAL_MAX];
    char model[EDGE_E7_MODEL_MAX];
    int w;

    peer[0] = '\0';
    serial[0] = '\0';
    model[0] = '\0';
    st = EDGE_E7_SESS_EMPTY;
    sess = NULL;
    if (ch) {
        edge_e7_session_t *s =
            session_find_by_mac((edge_e7_callhome_t *)ch, rs->mac);
        if (s) {
            sess = s;
            st = s->state;
            snprintf(peer, sizeof(peer), "%s", s->peer);
            snprintf(serial, sizeof(serial), "%s", s->identity.serial);
            snprintf(model, sizeof(model), "%s", s->identity.model);
        }
    }
    (void)sess;
    w = snprintf(buf + *off, buf_sz - *off,
                 "{\"mac\":\"%s\",\"label\":\"%s\",\"enabled\":%s,"
                 "\"from_yaml\":%s,\"session_state\":\"%s\","
                 "\"serial\":\"%s\",\"model\":\"%s\",\"peer\":\"%s\","
                 "\"probe_mode\":%u,\"probe\":\"%s\"}",
                 rs->mac, rs->label[0] ? rs->label : "",
                 rs->enabled ? "true" : "false",
                 rs->from_yaml ? "true" : "false", edge_e7_sess_state_name(st),
                 serial, model, peer, (unsigned)rs->probe_mode,
                 e7_probe_mode_name(rs->probe_mode));
    if (w < 0 || (size_t)w >= buf_sz - *off) {
        return -1;
    }
    *off += (size_t)w;
    return 0;
}

int edge_e7_callhome_shelves_json(const edge_e7_callhome_t *ch, char *buf,
                                  size_t buf_sz)
{
    size_t off = 0;
    uint32_t i;
    int first = 1;
    int w;

    if (!ch || !buf || buf_sz < 16) {
        return -1;
    }
    w = snprintf(buf, buf_sz,
                 "{\"v\":1,\"note\":\"allowlist not written to YAML; "
                 "use allowlist_path for file durability\","
                 "\"shelves\":[");
    if (w < 0 || (size_t)w >= buf_sz) {
        return -1;
    }
    off = (size_t)w;
    for (i = 0; i < EDGE_E7_RUNTIME_SHELVES_MAX; i++) {
        if (!ch->runtime[i].used) {
            continue;
        }
        if (!first) {
            if (off + 1 >= buf_sz) {
                return -1;
            }
            buf[off++] = ',';
            buf[off] = '\0';
        }
        first = 0;
        if (append_shelf_obj(ch, &ch->runtime[i], buf, buf_sz, &off) != 0) {
            return -1;
        }
    }
    if (off + 3 >= buf_sz) {
        return -1;
    }
    buf[off++] = ']';
    buf[off++] = '}';
    buf[off] = '\0';
    return (int)off;
}

int edge_e7_callhome_shelf_json(const edge_e7_callhome_t *ch, const char *mac,
                                char *buf, size_t buf_sz)
{
    char norm[EDGE_E7_MAC_MAX];
    const edge_e7_runtime_shelf_t *rs;
    edge_e7_session_t *sess;
    size_t off = 0;
    int w;

    if (!ch || !mac || !buf || buf_sz < 32) {
        return -1;
    }
    if (edge_e7_mac_normalize(mac, norm, sizeof(norm)) != 0) {
        return -1;
    }
    rs = NULL;
    {
        uint32_t i;
        for (i = 0; i < EDGE_E7_RUNTIME_SHELVES_MAX; i++) {
            if (ch->runtime[i].used && strcmp(ch->runtime[i].mac, norm) == 0) {
                rs = &ch->runtime[i];
                break;
            }
        }
    }
    sess = session_find_by_mac((edge_e7_callhome_t *)ch, norm);
    if (!rs && !sess) {
        return -2; /* not found */
    }
    w = snprintf(buf, buf_sz, "{\"v\":1,\"mac\":\"%s\",", norm);
    if (w < 0 || (size_t)w >= buf_sz) {
        return -1;
    }
    off = (size_t)w;
    if (rs) {
        w = snprintf(buf + off, buf_sz - off,
                     "\"configured\":true,\"label\":\"%s\",\"enabled\":%s,"
                     "\"from_yaml\":%s,"
                     "\"note\":\"allowlist_path for file durability\",",
                     rs->label[0] ? rs->label : "",
                     rs->enabled ? "true" : "false",
                     rs->from_yaml ? "true" : "false");
    } else {
        w = snprintf(buf + off, buf_sz - off,
                     "\"configured\":false,\"enabled\":false,");
    }
    if (w < 0 || (size_t)w >= buf_sz - off) {
        return -1;
    }
    off += (size_t)w;
    if (sess) {
        w = snprintf(buf + off, buf_sz - off,
                     "\"session_state\":\"%s\",\"serial\":\"%s\","
                     "\"model\":\"%s\",\"source_ip\":\"%s\",\"peer\":\"%s\","
                     "\"allowlisted\":%s,\"subscribed\":%s}",
                     edge_e7_sess_state_name(sess->state), sess->identity.serial,
                     sess->identity.model,
                     sess->identity.source_ip[0] ? sess->identity.source_ip
                                                 : sess->peer,
                     sess->peer, sess->allowlisted ? "true" : "false",
                     sess->sub_ok ? "true" : "false");
    } else {
        w = snprintf(buf + off, buf_sz - off,
                     "\"session_state\":\"empty\"}");
    }
    if (w < 0 || (size_t)w >= buf_sz - off) {
        return -1;
    }
    off += (size_t)w;
    return (int)off;
}

int edge_e7_callhome_allowlist_upsert(edge_e7_callhome_t *ch, const char *mac,
                                      const char *label, int enabled)
{
    char norm[EDGE_E7_MAC_MAX];
    edge_e7_runtime_shelf_t *rs;

    if (!ch || !mac) {
        return -1;
    }
    if (edge_e7_mac_normalize(mac, norm, sizeof(norm)) != 0) {
        return -1;
    }
    rs = runtime_find(ch, norm);
    if (!rs) {
        rs = runtime_alloc(ch);
        if (!rs) {
            return -1;
        }
        memcpy(rs->mac, norm, sizeof(rs->mac));
        rs->from_yaml = 0;
        rs->probe_mode = EDGE_E7_PROBE_SILENT;
    }
    if (label) {
        snprintf(rs->label, sizeof(rs->label), "%s", label);
    }
    rs->enabled = enabled ? 1 : 0;
    put_config_json(ch, rs);
    save_runtime_to_file(ch);
    return 0;
}

int edge_e7_callhome_allowlist_delete(edge_e7_callhome_t *ch, const char *mac)
{
    char norm[EDGE_E7_MAC_MAX];
    char mac_key[EDGE_E7_MAC_MAX];
    char key[EDGE_STATE_KEY_MAX];
    edge_e7_runtime_shelf_t *rs;
    edge_e7_session_t *s;

    if (!ch || !mac) {
        return -1;
    }
    if (edge_e7_mac_normalize(mac, norm, sizeof(norm)) != 0) {
        return -1;
    }
    rs = runtime_find(ch, norm);
    if (!rs) {
        /* still disconnect if live */
        s = session_find_by_mac(ch, norm);
        if (s) {
            session_close(ch, s);
            return 0;
        }
        return -1;
    }
    if (edge_e7_mac_to_key_seg(norm, mac_key, sizeof(mac_key)) == 0) {
        int n = snprintf(key, sizeof(key), "e7/%s/config", mac_key);
        if (n > 0 && (size_t)n < sizeof(key) && ch->state) {
            (void)edge_state_delete_and_notify(ch->state, ch->hub, "inventory",
                                               key, NULL, 0);
        }
    }
    memset(rs, 0, sizeof(*rs));
    if (ch->runtime_count > 0) {
        ch->runtime_count--;
    }
    s = session_find_by_mac(ch, norm);
    if (s) {
        session_close(ch, s);
    }
    save_runtime_to_file(ch);
    return 0;
}

int edge_e7_callhome_disconnect(edge_e7_callhome_t *ch, const char *mac)
{
    char norm[EDGE_E7_MAC_MAX];
    edge_e7_session_t *s;

    if (!ch || !mac) {
        return -1;
    }
    if (edge_e7_mac_normalize(mac, norm, sizeof(norm)) != 0) {
        return -1;
    }
    s = session_find_by_mac(ch, norm);
    if (s) {
        session_close(ch, s);
    }
    return 0;
}

static int count_inflight_cmds(const edge_e7_callhome_t *ch, int shelf_slot)
{
    uint32_t i;
    int n = 0;
    for (i = 0; i < EDGE_E7_CMD_TABLE_MAX; i++) {
        if (ch->cmds[i].used && !ch->cmds[i].complete &&
            ch->cmds[i].shelf_slot == shelf_slot) {
            n++;
        }
    }
    return n;
}

static edge_e7_cmd_slot_t *cmd_alloc(edge_e7_callhome_t *ch)
{
    uint32_t i;
    for (i = 0; i < EDGE_E7_CMD_TABLE_MAX; i++) {
        if (!ch->cmds[i].used || ch->cmds[i].complete) {
            memset(&ch->cmds[i], 0, sizeof(ch->cmds[i]));
            ch->cmds[i].used = 1;
            return &ch->cmds[i];
        }
    }
    return NULL;
}

int edge_e7_callhome_command_submit(edge_e7_callhome_t *ch, const char *mac,
                                    const char *rpc_xml, size_t rpc_len,
                                    const char *op, char *cmd_id_out,
                                    size_t cmd_id_sz, int *http_status)
{
    char norm[EDGE_E7_MAC_MAX];
    edge_e7_session_t *s;
    edge_e7_cmd_slot_t *cmd;
    int mid = -1;

    if (http_status) {
        *http_status = 400;
    }
    if (!ch || !mac || !cmd_id_out || cmd_id_sz < 8) {
        return -1;
    }
    if (edge_e7_mac_normalize(mac, norm, sizeof(norm)) != 0) {
        return -1;
    }
    s = session_find_by_mac(ch, norm);
    if (!s) {
        if (http_status) {
            *http_status = 409;
        }
        return -1;
    }
    if (s->state != EDGE_E7_SESS_OPEN) {
        if (http_status) {
            *http_status = 503;
        }
        return -1;
    }
#if !EDGEHOST_HAVE_LIBNETCONF
    (void)rpc_xml;
    (void)rpc_len;
    (void)op;
    if (http_status) {
        *http_status = 503;
    }
    return -1;
#else
    if (!s->nc) {
        if (http_status) {
            *http_status = 503;
        }
        return -1;
    }
    if (count_inflight_cmds(ch, s->slot) >= EDGE_E7_CMD_MAX_PER_SHELF) {
        if (http_status) {
            *http_status = 429;
        }
        ch->stats.commands_err++;
        return -1;
    }
    cmd = cmd_alloc(ch);
    if (!cmd) {
        if (http_status) {
            *http_status = 429;
        }
        ch->stats.commands_err++;
        return -1;
    }

    if (rpc_xml && rpc_len > 0) {
        mid = netconf_send_rpc(s->nc, rpc_xml, rpc_len);
    } else if (op && strcmp(op, "get-config") == 0) {
        mid = netconf_get_config(s->nc, "running", NULL);
    } else if (op && strcmp(op, "get") == 0) {
        mid = netconf_get(s->nc, NULL);
    } else {
        memset(cmd, 0, sizeof(*cmd));
        if (http_status) {
            *http_status = 400;
        }
        ch->stats.commands_err++;
        return -1;
    }
    if (mid < 0) {
        memset(cmd, 0, sizeof(*cmd));
        if (http_status) {
            *http_status = 503;
        }
        ch->stats.commands_err++;
        return -1;
    }
    (void)session_drain_output(s);
    ch->cmd_seq++;
    snprintf(cmd->cmd_id, sizeof(cmd->cmd_id), "c%08u", ch->cmd_seq);
    snprintf(cmd_id_out, cmd_id_sz, "%s", cmd->cmd_id);
    cmd->message_id = (uint32_t)mid;
    cmd->shelf_slot = s->slot;
    memcpy(cmd->mac, norm, sizeof(cmd->mac));
    cmd->deadline_ms = mono_now_ms() + 15000u;
    cmd->complete = 0;
    /* Pending stub in net.pon so GET can find it immediately. */
    {
        char mac_key[EDGE_E7_MAC_MAX];
        char key[EDGE_STATE_KEY_MAX];
        char json[256];
        int n;
        if (edge_e7_mac_to_key_seg(norm, mac_key, sizeof(mac_key)) == 0) {
            n = snprintf(key, sizeof(key), "e7/%s/cmd/%s", mac_key, cmd->cmd_id);
            if (n > 0 && (size_t)n < sizeof(key)) {
                n = snprintf(json, sizeof(json),
                             "{\"v\":1,\"cmd_id\":\"%s\",\"mac\":\"%s\","
                             "\"message_id\":%u,\"status\":\"pending\"}",
                             cmd->cmd_id, norm, (unsigned)mid);
                if (n > 0 && ch->state) {
                    (void)edge_state_put_and_notify(ch->state, ch->hub, "net.pon",
                                                    key, json, (size_t)n, NULL,
                                                    0);
                }
            }
        }
    }
    if (http_status) {
        *http_status = 202;
    }
    return 0;
#endif
}

int edge_e7_callhome_command_json(const edge_e7_callhome_t *ch, const char *mac,
                                  const char *cmd_id, char *buf, size_t buf_sz)
{
    char norm[EDGE_E7_MAC_MAX];
    char mac_key[EDGE_E7_MAC_MAX];
    char key[EDGE_STATE_KEY_MAX];
    size_t vlen = 0;
    edge_state_err_t e;
    int n;

    if (!ch || !mac || !cmd_id || !buf || buf_sz < 16) {
        return -1;
    }
    if (edge_e7_mac_normalize(mac, norm, sizeof(norm)) != 0 ||
        edge_e7_mac_to_key_seg(norm, mac_key, sizeof(mac_key)) != 0) {
        return -1;
    }
    n = snprintf(key, sizeof(key), "e7/%s/cmd/%s", mac_key, cmd_id);
    if (n < 0 || (size_t)n >= sizeof(key)) {
        return -1;
    }
    if (!ch->state) {
        return -1;
    }
    e = edge_state_get(ch->state, "net.pon", key, buf, buf_sz, &vlen);
    if (e != EDGE_STATE_OK) {
        return -1;
    }
    if (vlen + 1 < buf_sz) {
        buf[vlen] = '\0';
    } else if (buf_sz > 0) {
        buf[buf_sz - 1] = '\0';
    }
    return (int)vlen;
}

int edge_e7_callhome_onts_json(const edge_e7_callhome_t *ch, const char *mac,
                               const char *cursor, size_t limit, char *buf,
                               size_t buf_sz)
{
    char norm[EDGE_E7_MAC_MAX];
    char mac_key[EDGE_E7_MAC_MAX];
    char prefix[EDGE_STATE_KEY_MAX];
    char keys[64][EDGE_STATE_KEY_MAX];
    int nkeys, i;
    size_t off = 0;
    int w;
    int first = 1;
    size_t take;

    if (!ch || !mac || !buf || buf_sz < 32) {
        return -1;
    }
    if (edge_e7_mac_normalize(mac, norm, sizeof(norm)) != 0 ||
        edge_e7_mac_to_key_seg(norm, mac_key, sizeof(mac_key)) != 0) {
        return -1;
    }
    if (!ch->state) {
        return -1;
    }
    take = limit ? limit : 64;
    if (take > 64) {
        take = 64;
    }
    w = snprintf(prefix, sizeof(prefix), "e7/%s/ont/", mac_key);
    if (w < 0 || (size_t)w >= sizeof(prefix)) {
        return -1;
    }
    nkeys = edge_state_list(ch->state, "net.pon", prefix, keys, 64);
    if (nkeys < 0) {
        nkeys = 0;
    }
    w = snprintf(buf, buf_sz, "{\"v\":1,\"mac\":\"%s\",\"onts\":[", norm);
    if (w < 0 || (size_t)w >= buf_sz) {
        return -1;
    }
    off = (size_t)w;
    for (i = 0; i < nkeys && (size_t)i < take; i++) {
        const char *k = keys[i];
        /* cursor: skip keys <= cursor (exclusive next page) */
        if (cursor && cursor[0] && strcmp(k, cursor) <= 0) {
            continue;
        }
        if (!first) {
            if (off + 1 >= buf_sz) {
                return -1;
            }
            buf[off++] = ',';
        }
        first = 0;
        {
            char val[EDGE_STATE_VALUE_DEFAULT];
            size_t vlen = 0;
            edge_state_err_t e =
                edge_state_get(ch->state, "net.pon", k, val, sizeof(val), &vlen);
            if (e == EDGE_STATE_OK && vlen > 0 && off + vlen + 16 < buf_sz) {
                w = snprintf(buf + off, buf_sz - off,
                             "{\"key\":\"%s\",\"value\":", k);
                if (w < 0) {
                    break;
                }
                off += (size_t)w;
                memcpy(buf + off, val, vlen);
                off += vlen;
                buf[off++] = '}';
                buf[off] = '\0';
            } else {
                w = snprintf(buf + off, buf_sz - off, "{\"key\":\"%s\"}", k);
                if (w < 0 || (size_t)w >= buf_sz - off) {
                    return -1;
                }
                off += (size_t)w;
            }
        }
    }
    if (off + 2 >= buf_sz) {
        return -1;
    }
    buf[off++] = ']';
    buf[off++] = '}';
    buf[off] = '\0';
    return (int)off;
}
