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

#define EDGE_E7_DIRTY_NS_MAX     32

typedef struct {
    edge_e7_sess_state_t state;
    int                  fd;
    int                  slot;
    edge_e7_identity_t   identity;
    char                 peer[EDGE_E7_PEER_ADDR_MAX];
    int                  allowlisted; /* MAC in shelves[] and enabled */
    int                  auto_unknown; /* accepted via auto_subscribe_unknown */
    int                  sub_sent;     /* create_subscription issued */
    int                  sub_ok;       /* subscription rpc-ok */
    int                  sub_msg_id;
    int                  hello_sent;   /* netconf_send_hello issued */
    int                  use_ssh;      /* transport=ssh for this session */

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
    case EDGE_E7_SESS_SSH:       return "ssh";
    case EDGE_E7_SESS_HELLO:     return "hello";
    case EDGE_E7_SESS_OPEN:      return "open";
    case EDGE_E7_SESS_ERROR:     return "error";
    case EDGE_E7_SESS_CLOSING:   return "closing";
    default:                     return "unknown";
    }
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
    int was_open;

    if (!s || s->state == EDGE_E7_SESS_EMPTY) {
        return;
    }
    was_open = (s->state == EDGE_E7_SESS_OPEN);
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

static void session_try_subscribe(edge_e7_callhome_t *ch, edge_e7_session_t *s)
{
    int mid;

    if (!s || !s->nc || s->sub_sent || s->state != EDGE_E7_SESS_OPEN) {
        return;
    }
    /* Allowlisted or accepted via auto_subscribe_unknown. */
    if (!s->allowlisted && !s->auto_unknown) {
        return;
    }
    mid = netconf_create_subscription(s->nc, "NETCONF", NULL);
    if (mid < 0) {
        return;
    }
    s->sub_sent = 1;
    s->sub_msg_id = mid;
    (void)session_drain_output(s);
    (void)ch;
}

static void on_notification(edge_e7_callhome_t *ch, edge_e7_session_t *s,
                            const netconf_notification_t *n)
{
    edge_e7_apply_err_t ae;
    char key[EDGE_STATE_KEY_MAX];

    if (!ch || !s || !n || n->xml_len == 0) {
        return;
    }
    ch->stats.notifications++;
    if (!ch->state || !s->identity.identity_ok) {
        return;
    }
    ae = edge_e7_event_apply_lab_v1(ch->state, s->identity.mac, n->xml,
                                    n->xml_len);
    if (ae != EDGE_E7_APPLY_OK) {
        return;
    }
    ch->stats.state_puts++;
    if (lab_v1_state_key(s->identity.mac, n->xml, n->xml_len, key,
                         sizeof(key)) == 0) {
        notify_coalesce(ch, "net.pon", key);
    }
    /* PR-9: coalesce map.dynamic ONT mirror when coords present (apply put both). */
    if (lab_v1_map_key(s->identity.mac, n->xml, n->xml_len, key, sizeof(key)) ==
        0) {
        notify_coalesce(ch, "map.dynamic", key);
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
        } else if (ev.type == NETCONF_EVENT_RPC_OK ||
                   ev.type == NETCONF_EVENT_RPC_REPLY) {
            on_rpc_reply(ch, s, &ev.data.rpc_reply);
        }
        /* QUEUE_OVERFLOW / ERROR: count optionally later */
    }
}

#if EDGEHOST_E7_SSH_AVAILABLE
/**
 * Apply SSH Call Home fields from edge config onto a reduced profile.
 * NMS after accept: NETCONF_ROLE_CLIENT + NETCONF_SSH_CALLHOME.
 */
static void edge_e7_netconf_apply_ssh(netconf_config_t *ncfg,
                                      const edge_config_t *cfg)
{
    if (!ncfg || !cfg) {
        return;
    }
    ncfg->ssh_mode = NETCONF_SSH_CALLHOME;
    if (cfg->e7_ssh_password[0]) {
        ncfg->ssh_server_password = cfg->e7_ssh_password;
    } else {
        ncfg->ssh_server_password = NULL;
    }
    if (cfg->e7_ssh_username[0]) {
        ncfg->ssh_server_username = cfg->e7_ssh_username;
    } else {
        ncfg->ssh_server_username = NULL;
    }
    if (cfg->e7_host_key_path[0]) {
        ncfg->host_key_path = cfg->e7_host_key_path;
    } else {
        ncfg->host_key_path = NULL; /* ephemeral ed25519 */
    }
    ncfg->ssh_allow_none_auth = cfg->e7_ssh_allow_none_auth ? 1 : 0;
}
#endif /* EDGEHOST_E7_SSH_AVAILABLE */

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
    /* Raw: ready immediately after create. SSH: wait for subsystem. */
    if (s->use_ssh && st != NETCONF_STATE_CAPABILITY_EXCHANGE &&
        st != NETCONF_STATE_SESSION_OPEN) {
        return 0; /* still SSH_CONNECTING / SSH_AUTHENTICATING */
    }
    if (netconf_send_hello(s->nc, NULL, 0) != 0) {
        return -1;
    }
    s->hello_sent = 1;
    s->state = EDGE_E7_SESS_HELLO;
    drain_nc_events(ch, s);
    if (session_drain_output(s) != 0) {
        return -1;
    }
    return 0;
}

static int session_start_netconf(edge_e7_callhome_t *ch, edge_e7_session_t *s)
{
    netconf_config_t ncfg;
    int use_ssh = 0;

    if (!ch || !ch->cfg || !s) {
        return -1;
    }
    edge_e7_netconf_profile(&ncfg);
    use_ssh = e7_transport_is_ssh(ch->cfg->e7_transport);
    s->use_ssh = use_ssh ? 1 : 0;
    s->hello_sent = 0;

    if (use_ssh) {
#if !EDGEHOST_E7_SSH_AVAILABLE
        return -1;
#else
        edge_e7_netconf_apply_ssh(&ncfg, ch->cfg);
#endif
    }

    s->nc = netconf_create_with_config(NETCONF_ROLE_CLIENT, &ncfg);
    if (!s->nc) {
        return -1;
    }

    /* SSH init may already have produced banner/KEX WRITE — drain it. */
    drain_nc_events(ch, s);
    if (session_drain_output(s) != 0) {
        session_clear_nc(s);
        return -1;
    }

    s->identity_ms = mono_now_ms();
    if (use_ssh) {
        /* Wait for subsystem before netconf_send_hello (raw TCP identity done). */
        s->state = EDGE_E7_SESS_SSH;
        return 0;
    }

    if (session_try_send_hello(ch, s) != 0) {
        session_clear_nc(s);
        return -1;
    }
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
        session_try_subscribe(ch, s);
    }
}

/** Post feed/drain: maybe finish SSH → hello, then check OPEN. */
static void session_after_nc_io(edge_e7_callhome_t *ch, edge_e7_session_t *s)
{
    if (!s || !s->nc) {
        return;
    }
    if (!s->hello_sent) {
        if (session_try_send_hello(ch, s) != 0) {
            ch->stats.rejects_other++;
            put_session_json(ch, s, "error");
            session_close(ch, s);
            return;
        }
    }
    if (s->state == EDGE_E7_SESS_EMPTY) {
        return;
    }
    if (session_drain_output(s) != 0) {
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
        session_close(ch, s);
        return -1;
    }
    s->identity = id;

    look = shelf_lookup_runtime(ch, s->identity.mac);
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
    /*
     * Feed any leftover post-identity bytes into libnetconf.
     * Raw: start of peer NETCONF hello. SSH: first ciphertext (SSH-2.0-…).
     */
    if (s->id_len > 0 && s->nc) {
        size_t used =
            netconf_feed_input(s->nc, (const uint8_t *)s->id_buf, s->id_len);
        (void)used;
        s->id_len = 0;
        drain_nc_events(ch, s);
        session_after_nc_io(ch, s);
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
    /* SSH (post-identity) or NETCONF hello/open: feed into libnetconf SM. */
    if ((s->state == EDGE_E7_SESS_SSH || s->state == EDGE_E7_SESS_HELLO ||
         s->state == EDGE_E7_SESS_OPEN) &&
        s->nc) {
        size_t used = netconf_feed_input(s->nc, s->rx, (size_t)n);
        (void)used;
        drain_nc_events(ch, s);
        session_after_nc_io(ch, s);
        if (s->state == EDGE_E7_SESS_EMPTY) {
            return;
        }
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
    } else if (s->state == EDGE_E7_SESS_SSH || s->state == EDGE_E7_SESS_HELLO) {
        uint64_t base = s->identity_ms ? s->identity_ms : s->accepted_ms;
        if (mono_ms > base && mono_ms - base > EDGE_E7_HELLO_TIMEOUT_MS) {
            ch->stats.rejects_other++;
            put_session_json(ch, s,
                             s->state == EDGE_E7_SESS_SSH ? "ssh_timeout"
                                                          : "hello_timeout");
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
    uint32_t dirty_cap;
    size_t est;

#if !EDGEHOST_HAVE_LIBNETCONF
    (void)opts;
    return NULL;
#else
    if (!opts || !opts->cfg || !opts->cfg->e7_enabled) {
        return NULL;
    }
    if (e7_transport_is_ssh(opts->cfg->e7_transport)) {
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

    if (e7_transport_is_ssh(ch->cfg->e7_transport)) {
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
            e7_transport_is_ssh(ch->cfg->e7_transport) ? "ssh" : "raw");
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
        "\"e7_rss_estimate\":%zu,"
        "\"e7_subscriptions_ok\":%llu,"
        "\"max_sessions\":%u,"
        "\"runtime_shelves\":%u"
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
        rss_est,
        (unsigned long long)st->subscriptions_ok,
        ch->max_sessions, ch->runtime_count);
    return (n < 0 || (size_t)n >= buf_sz) ? -1 : n;
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
                 "\"serial\":\"%s\",\"model\":\"%s\",\"peer\":\"%s\"}",
                 rs->mac, rs->label[0] ? rs->label : "",
                 rs->enabled ? "true" : "false",
                 rs->from_yaml ? "true" : "false", edge_e7_sess_state_name(st),
                 serial, model, peer);
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
