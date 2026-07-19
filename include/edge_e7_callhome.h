/**
 * @file edge_e7_callhome.h
 * @brief E7 NETCONF Call Home listen, identity, subscribe, lab.v1 apply (PR-4b).
 *
 * Host owns TCP accept + Calix identity parse (MAC primary). After identity,
 * libnetconf NETCONF_ROLE_CLIENT runs delimiter-framed hellos to SESSION_OPEN,
 * then create_subscription (allowlisted / auto_subscribe_unknown). Notifications
 * apply into net.pon; inventory/session uses put_and_notify. K16 dirty-set
 * coalesces high-rate ONT/PON STATE_CHANGED (flush ≤100 ms on tick).
 *
 * Requires EDGEHOST_HAVE_LIBNETCONF (sibling libnetconf). Without it, create
 * returns NULL.
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

/** Session table capacity (design: ~150, default max_sessions 160). */
#define EDGE_E7_MAX_SESSIONS 160
/** Peer TCP address string (inet_ntop). */
#define EDGE_E7_PEER_ADDR_MAX 64
/** Identity preamble accumulation cap. */
#define EDGE_E7_IDENTITY_BUF_MAX 2048
/** Default dirty-set cap when config e7_dirty_cap is 0 (K16). */
#define EDGE_E7_DIRTY_CAP_DEFAULT 8192
/** Coalesce flush interval (ms) — design ≤100. */
#define EDGE_E7_COALESCE_MS 100u

typedef enum {
    EDGE_E7_SESS_EMPTY = 0,
    EDGE_E7_SESS_ACCEPTED,
    EDGE_E7_SESS_IDENTITY, /* reading Calix identity preamble */
    EDGE_E7_SESS_SSH,      /* production only; unused in raw lab */
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
} edge_e7_callhome_stats_t;

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
 * e7 disabled, max_sessions==0, or RSS budget exceeded by estimate.
 */
edge_e7_callhome_t *edge_e7_callhome_create(const edge_e7_callhome_opts_t *opts);

void edge_e7_callhome_destroy(edge_e7_callhome_t *ch);

/**
 * Attach or replace WS hub (not owned). Used by iouring after hub create so
 * Call Home can fan out STATE_CHANGED (K16).
 */
void edge_e7_callhome_set_hub(edge_e7_callhome_t *ch, edge_ws_hub_t *hub);

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
 */
void edge_e7_netconf_profile(void *cfg_out /* netconf_config_t * when linked */);

/** Rough per-session RSS estimate used for budget check (bytes). */
size_t edge_e7_session_rss_estimate(void);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_E7_CALLHOME_H */
