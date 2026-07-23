/**
 * @file edge_e7_callhome.h
 * @brief E7 NETCONF Call Home listen, identity, subscribe, lab.v1 apply, REST (PR-5).
 *
 * Host owns TCP accept + Calix identity parse (MAC primary). After identity,
 * libnetconf NETCONF_ROLE_CLIENT runs delimiter-framed hellos to SESSION_OPEN,
 * then create_subscription (allowlisted / auto_subscribe_unknown). Notifications
 * apply into net.pon (+ map.dynamic when lon/lat present); inventory/session
 * uses put_and_notify. K16 dirty-set coalesces high-rate ONT/PON (and map)
 * STATE_CHANGED (flush ≤100 ms on tick).
 *
 * REST host APIs (PR-5) expose status/shelves/commands for HTTP /api/v1/e7/.
 * Runtime allowlist upsert is non-durable (not written back to YAML).
 * SIGHUP: edge_e7_callhome_apply_config with reload_policy merge|replace_all.
 *
 * Requires EDGEHOST_HAVE_LIBNETCONF (sibling libnetconf). Without it, create
 * returns NULL. transport=ssh requires EDGEHOST_E7_SSH_AVAILABLE
 * (libchssh preferred, or legacy libnetconf+libassh).
 */
#ifndef EDGE_E7_CALLHOME_H
#define EDGE_E7_CALLHOME_H

#include "edge_config.h"
#include "edge_e7_event_apply.h"
#include "edge_state.h"
#include "edge_ws.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef EDGEHOST_HAVE_LIBNETCONF
#define EDGEHOST_HAVE_LIBNETCONF 0
#endif

/**
 * SSH Call Home available (CMake). Prefer libchssh (EDGEHOST_E7_CHSSH_AVAILABLE);
 * legacy path is libnetconf+libassh. When 0, transport: ssh create/bind fails.
 */
#ifndef EDGEHOST_E7_SSH_AVAILABLE
#define EDGEHOST_E7_SSH_AVAILABLE 0
#endif

/** 1 when sibling libchssh is linked (preferred SSH Call Home transport). */
#ifndef EDGEHOST_E7_CHSSH_AVAILABLE
#define EDGEHOST_E7_CHSSH_AVAILABLE 0
#endif

/** Session table capacity (design: ~150, default max_sessions 160). */
#define EDGE_E7_MAX_SESSIONS 160
/** Peer TCP address string (inet_ntop). */
#define EDGE_E7_PEER_ADDR_MAX 64
/** Identity preamble accumulation cap. */
/** Identity preamble buffer (Calix XML or Junos DEVICE-CONN-INFO + HOST-KEY). */
#define EDGE_E7_IDENTITY_BUF_MAX 8192
/** Default dirty-set cap when config e7_dirty_cap is 0 (K16). */
#define EDGE_E7_DIRTY_CAP_DEFAULT 8192
/** Coalesce flush interval (ms) — design ≤100. */
#define EDGE_E7_COALESCE_MS 100u
/** Max in-flight placeholder commands per shelf (design). */
#define EDGE_E7_CMD_MAX_PER_SHELF 4
/** cmd_id buffer (e.g. "c00000001") + NUL. */
#define EDGE_E7_CMD_ID_MAX 24
/** Global pending command correlation slots. */
#define EDGE_E7_CMD_TABLE_MAX 64
/** Runtime allowlist capacity (YAML seed + REST upserts). */
#define EDGE_E7_RUNTIME_SHELVES_MAX EDGE_CONFIG_E7_SHELVES_MAX
/** In-memory connection progress ring (GET /api/v1/e7/events). */
#define EDGE_E7_TRACE_CAP 64
/** Stage token max (e.g. "identity_ok", "ssh_timeout"). */
#define EDGE_E7_TRACE_STAGE_MAX 32
/** Free-form detail max (peer notes, hex dumps, reject reason). */
#define EDGE_E7_TRACE_DETAIL_MAX 200

typedef enum {
    EDGE_E7_SESS_EMPTY = 0,
    EDGE_E7_SESS_ACCEPTED,
    EDGE_E7_SESS_IDENTITY, /* reading Calix identity preamble */
    EDGE_E7_SESS_POST_ID,  /* identity ok; choosing/waiting transport */
    EDGE_E7_SESS_SSH,      /* SSH Call Home handshake (NMS = SSH server) */
    EDGE_E7_SESS_HELLO,    /* NETCONF hellos in progress */
    EDGE_E7_SESS_OPEN,     /* netconf SESSION_OPEN */
    EDGE_E7_SESS_ERROR,
    EDGE_E7_SESS_CLOSING
} edge_e7_sess_state_t;

typedef struct {
    uint64_t accepts;
    uint64_t rejects_bad_identity;
    uint64_t rejects_not_allowlisted;
    uint64_t rejects_disabled;
    uint64_t rejects_capacity;
    uint64_t rejects_other;
    uint64_t sessions_open;   /* currently OPEN */
    uint64_t sessions_opened; /* cumulative OPEN transitions */
    uint64_t notifications;   /* NOTIFICATION events applied or seen */
    uint64_t state_puts;      /* successful inventory/net.pon puts from CH */
    uint64_t coalesce_flush;  /* dirty-set flush cycles */
    uint64_t coalesce_overflow; /* dirty table full → force notify */
    uint64_t subscriptions_ok; /* create-subscription rpc-ok */
    uint64_t commands_ok;     /* command RPC completed ok */
    uint64_t commands_err;    /* command submit/reply errors */
    uint64_t ws_fanouts;      /* STATE_CHANGED enqueues from CH path */
} edge_e7_callhome_stats_t;

/**
 * One runtime allowlist entry (YAML seed + REST). Non-durable across restart
 * unless re-seeded from YAML.
 */
typedef struct {
    int  used;
    char mac[EDGE_E7_MAC_MAX]; /* normalized colon form (Calix); empty for Junos-only */
    char label[EDGE_CONFIG_E7_SHELF_ID_MAX];
    int  enabled; /* 1 allow subscribe; 0 present but disabled */
    int  from_yaml;
    /** "calix" | "junos" | "" (auto/unknown). */
    char vendor[EDGE_E7_VENDOR_MAX];
    /** Junos DEVICE-ID (outbound-ssh device-id). */
    char device_id[EDGE_E7_DEVICE_ID_MAX];
    /**
     * Optional shared secret matching Junos outbound-ssh `secret` for HMAC
     * verification of HOST-KEY. Empty = do not require HMAC (secret optional).
     */
    char secret[EDGE_E7_SECRET_MAX];
    /**
     * Post-identity probe strategy for the next dial (raw Call Home).
     * Advanced on peer_eof before SESSION_OPEN so successive E7 retries
     * explore silent / identity-ACK / client-hello / server-hello.
     */
    uint8_t probe_mode;
    /**
     * transport:ssh field ladder start phase for the next dial
     * (EDGE_E7_SSH_PHASE_*). Advanced when peer closes during hold/ACK so
     * later redials skip failed identity ACKs and still try delayed SSH.
     */
    uint8_t ssh_field_next;
} edge_e7_runtime_shelf_t;

typedef struct edge_e7_callhome edge_e7_callhome_t;

/**
 * Create options. @p cfg is not copied — must outlive the instance.
 * state/hub optional (not owned). hub may be NULL (put-only); set later via
 * edge_e7_callhome_set_hub when the io_uring hub is created.
 */
typedef struct {
    const edge_config_t *cfg;
    edge_state_store_t  *state;
    edge_ws_hub_t       *hub;
} edge_e7_callhome_opts_t;

/**
 * Create Call Home engine. Fails (NULL) if libnetconf missing, cfg NULL,
 * e7 disabled, max_sessions==0, RSS budget exceeded by estimate, or
 * transport=ssh while EDGEHOST_E7_SSH_AVAILABLE is 0.
 */
edge_e7_callhome_t *edge_e7_callhome_create(const edge_e7_callhome_opts_t *opts);

void edge_e7_callhome_destroy(edge_e7_callhome_t *ch);

/**
 * Apply reloaded config allowlist (K15 / SIGHUP).
 *
 * Uses new_cfg->e7_reload_policy:
 *   - merge (default): for each YAML shelf, upsert runtime (YAML MAC wins for
 *     listed entries); runtime-only shelves not in YAML are retained.
 *   - replace_all: clear runtime table and reseed from YAML.
 *
 * Does not tear down the listen socket. If host/port/enabled differ from the
 * bound listen (or previously applied cfg), logs a warning that restart is
 * required; allowlist is still applied.
 *
 * @p new_cfg is not copied — must outlive the instance (typically
 * edgecore_config after shadow apply). Updates the internal cfg pointer.
 * @return 0 ok, -1 bad args.
 */
int edge_e7_callhome_apply_config(edge_e7_callhome_t *ch,
                                  const edge_config_t *new_cfg);

/**
 * Attach or replace WS hub (not owned). Used by iouring after hub create so
 * Call Home can fan out STATE_CHANGED (K16).
 */
void edge_e7_callhome_set_hub(edge_e7_callhome_t *ch, edge_ws_hub_t *hub);

/**
 * Optional ClickHouse async writer (not owned). When set, NETCONF notifications
 * are also enqueued as JSONEachRow into e7_netconf_events (aggregated flush).
 */
struct edge_clickhouse; /* edge_clickhouse.h */
void edge_e7_callhome_set_clickhouse(edge_e7_callhome_t *ch,
                                     struct edge_clickhouse *clickhouse);

/** 1 if instance exists and cfg.e7_enabled. */
int edge_e7_callhome_enabled(const edge_e7_callhome_t *ch);

/**
 * Bind/listen on plugins.e7_callhome.listen_host:port (non-blocking).
 * @return 0 ok, -1 on error.
 */
int edge_e7_callhome_bind(edge_e7_callhome_t *ch);

/** Listening fd after successful bind, or -1. */
int edge_e7_callhome_listen_fd(const edge_e7_callhome_t *ch);

/**
 * Take an already-accepted client fd (SOCK_CLOEXEC, will be set non-blocking).
 * Peer optional for inventory JSON.
 * @return 0 ok, -1 capacity/error (fd closed on failure).
 */
int edge_e7_callhome_on_accept(edge_e7_callhome_t *ch, int fd,
                               const struct sockaddr *peer, socklen_t peer_len);

/**
 * Non-blocking accept on listen_fd (if any) + pump all sessions (read/write/
 * identity/NETCONF) + dirty-set coalesce flush (≤100 ms). Safe to call from
 * io_uring tick or a poll loop.
 * @p mono_ms is CLOCK_MONOTONIC milliseconds.
 */
void edge_e7_callhome_poll(edge_e7_callhome_t *ch, uint64_t mono_ms);

/** Alias for poll — design name used by iouring tick. */
void edge_e7_callhome_on_tick(edge_e7_callhome_t *ch, uint64_t mono_ms);

/**
 * Close all sessions and the listen socket (optional; destroy also closes).
 * Does not free the object.
 */
void edge_e7_callhome_close_all(edge_e7_callhome_t *ch);

const edge_e7_callhome_stats_t *edge_e7_callhome_stats(
    const edge_e7_callhome_t *ch);

/**
 * Look up session state by MAC (any accepted normalize form).
 * Returns EDGE_E7_SESS_EMPTY if not found / bad mac.
 */
edge_e7_sess_state_t edge_e7_callhome_session_state_by_mac(
    const edge_e7_callhome_t *ch, const char *mac);

/** Count of sessions currently in OPEN. */
uint32_t edge_e7_callhome_open_count(const edge_e7_callhome_t *ch);

/**
 * Normative reduced libnetconf profile (K14). Exposed for tests.
 * event_queue_size=8, max_rpc/output 256 KiB, max_notification 64 KiB.
 * Does not set SSH fields — session_start applies CALLHOME mode from
 * edge_config when transport=ssh.
 */
void edge_e7_netconf_profile(void *cfg_out /* netconf_config_t * when linked */);

/** Rough per-session RSS estimate used for budget check (bytes). */
size_t edge_e7_session_rss_estimate(void);

/**
 * Module object JSON for GET /api/v1/debug/memory (no outer array).
 * Includes fixed tables, per live session, and per runtime shelf costs.
 * @return bytes written excl NUL, or -1.
 */
int edge_e7_callhome_memory_json(const edge_e7_callhome_t *ch, char *buf,
                                 size_t buf_sz);

/* ---- REST host APIs (PR-5) ---- */

/**
 * Build GET /api/v1/e7/status JSON into @p buf (NUL-terminated).
 * @return bytes written excl NUL, or -1.
 */
int edge_e7_callhome_status_json(const edge_e7_callhome_t *ch, char *buf,
                                 size_t buf_sz);

/**
 * Build GET /api/v1/e7/events JSON: live in-flight sessions + ring of
 * connection progress events (accept → identity → ssh → hello → open / reject).
 *
 * @p since_id: only include events with id > since_id (0 = all retained).
 * @return bytes written excl NUL, or -1.
 */
int edge_e7_callhome_events_json(const edge_e7_callhome_t *ch, uint64_t since_id,
                                 char *buf, size_t buf_sz);

/**
 * Build GET /api/v1/e7/shelves JSON (config allowlist + live session).
 * @return bytes written excl NUL, or -1.
 */
int edge_e7_callhome_shelves_json(const edge_e7_callhome_t *ch, char *buf,
                                  size_t buf_sz);

/**
 * Build GET /api/v1/e7/shelves/{mac} detail JSON.
 * @return bytes written excl NUL, or -1 (bad mac / not found → -2).
 */
int edge_e7_callhome_shelf_json(const edge_e7_callhome_t *ch, const char *mac,
                                char *buf, size_t buf_sz);

/**
 * Runtime allowlist upsert (non-durable). Also writes
 * inventory/e7/{mac_key}/config with a durability note.
 * @p label may be NULL; @p enabled 0/1.
 * @return 0 ok, -1 bad mac / full / error.
 */
int edge_e7_callhome_allowlist_upsert(edge_e7_callhome_t *ch, const char *mac,
                                      const char *label, int enabled);

/**
 * Extended upsert for multi-vendor Call Home.
 * @p mac may be a Calix MAC or a Junos DEVICE-ID (path key).
 * @p vendor "calix" | "junos" | NULL (default calix if MAC-shaped).
 * @p device_id optional Junos id (defaults to @p mac when vendor=junos).
 * @p secret optional shared secret (NULL = leave existing; "" = clear).
 */
int edge_e7_callhome_allowlist_upsert_ex(edge_e7_callhome_t *ch,
                                         const char *mac, const char *label,
                                         int enabled, const char *vendor,
                                         const char *device_id,
                                         const char *secret);

/**
 * Remove runtime allowlist entry + inventory config; disconnect live session.
 * @return 0 ok, -1 not found / bad mac.
 */
int edge_e7_callhome_allowlist_delete(edge_e7_callhome_t *ch, const char *mac);

/**
 * Force-close session for MAC if any (no allowlist change).
 * @return 0 closed or already absent, -1 bad mac.
 */
int edge_e7_callhome_disconnect(edge_e7_callhome_t *ch, const char *mac);

/**
 * Placeholder command: send inner rpc_xml (or op shortcut) on OPEN session.
 * Tracks message_id → cmd_id; result lands under net.pon e7/{mac}/cmd/{cmd_id}.
 *
 * @p rpc_xml: inner NETCONF op body (not outer &lt;rpc&gt;). If NULL/empty and
 * @p op is "get-config", sends netconf_get_config(running, NULL).
 * @p cmd_id_out receives the cmd id on success.
 * @p http_status set to suggested HTTP status (202/409/503/429/400).
 * @return 0 accepted, -1 rejected (see http_status).
 */
int edge_e7_callhome_command_submit(edge_e7_callhome_t *ch, const char *mac,
                                    const char *rpc_xml, size_t rpc_len,
                                    const char *op, char *cmd_id_out,
                                    size_t cmd_id_sz, int *http_status);

/**
 * Build GET command result JSON from net.pon (or pending in-memory).
 * @return bytes written excl NUL, or -1 not found / bad args.
 */
int edge_e7_callhome_command_json(const edge_e7_callhome_t *ch, const char *mac,
                                  const char *cmd_id, char *buf, size_t buf_sz);

/**
 * Paginated ONT list for shelf (keys under net.pon e7/{mac_key}/ont/).
 * Optional cursor is a key prefix skip (exclusive); limit 0 → default 64.
 * @return bytes written excl NUL, or -1.
 */
int edge_e7_callhome_onts_json(const edge_e7_callhome_t *ch, const char *mac,
                               const char *cursor, size_t limit, char *buf,
                               size_t buf_sz);

/** Session state name for JSON (static string). */
const char *edge_e7_sess_state_name(edge_e7_sess_state_t st);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_E7_CALLHOME_H */
