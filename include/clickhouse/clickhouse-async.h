/**
 * @file clickhouse-async.h
 * @brief ClickHouse-C async insert client (HTTP JSONEachRow batching).
 *
 * Provides the clickhouse-async surface used by edgehost: queue JSON rows,
 * aggregate for efficiency, flush on size/time via POST INSERT … FORMAT
 * JSONEachRow. Pure C; uses edgehost outbound HTTP.
 *
 * Layout: include as "clickhouse/clickhouse-async.h" (or clickhouse-async.h
 * when EDGEHOST_CLICKHOUSE_INCLUDE is on the include path).
 */
#ifndef CLICKHOUSE_ASYNC_H
#define CLICKHOUSE_ASYNC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CH_ASYNC_HOST_MAX     256
#define CH_ASYNC_DB_MAX       64
#define CH_ASYNC_USER_MAX     64
#define CH_ASYNC_PASSWORD_MAX 128
#define CH_ASYNC_TABLE_MAX    128
#define CH_ASYNC_URL_MAX      512

/** Connection + batch policy. */
typedef struct {
    char     host[CH_ASYNC_HOST_MAX]; /* hostname or IP */
    uint16_t port;                    /* default 8123 (HTTP) */
    char     database[CH_ASYNC_DB_MAX];
    char     user[CH_ASYNC_USER_MAX];
    char     password[CH_ASYNC_PASSWORD_MAX];
    int      use_https;               /* 0 HTTP, 1 HTTPS */
    /**
     * Optional full base URL override (http://host:8123/). When set, host/port
     * /use_https are ignored for the request URL.
     */
    char     base_url[CH_ASYNC_URL_MAX];
    uint32_t timeout_ms;              /* per flush HTTP timeout; 0 → 15000 */
    uint32_t flush_interval_ms;       /* time-based flush; 0 → 1000 */
    uint32_t flush_max_rows;          /* row count flush; 0 → 256 */
    size_t   flush_max_bytes;         /* payload flush; 0 → 256 KiB */
    int      allow_blocking_dns;      /* lab only */
} ch_async_options_t;

typedef struct {
    uint64_t rows_queued;
    uint64_t rows_flushed;
    uint64_t flush_ok;
    uint64_t flush_err;
    uint64_t bytes_flushed;
    uint64_t last_http_status;
    char     last_error[160];
} ch_async_stats_t;

typedef struct ch_async_client ch_async_client_t;

void ch_async_options_defaults(ch_async_options_t *o);

/**
 * Create client (allocates batch buffer). NULL on OOM / bad opts.
 * Does not open a persistent TCP connection — each flush is one HTTP POST.
 */
ch_async_client_t *ch_async_create(const ch_async_options_t *opt);

void ch_async_destroy(ch_async_client_t *c);

/**
 * Queue one JSON object (no trailing newline required) for @p table
 * (optionally database.table). Flushes when batch limits hit.
 * @return 0 ok, -1 error (see stats.last_error).
 */
int ch_async_insert_json_row(ch_async_client_t *c, const char *table,
                             const char *json_row, size_t json_len);

/** Force flush of pending rows for all tables (currently single active table). */
int ch_async_flush(ch_async_client_t *c);

/** Time-based flush (call from host tick). */
void ch_async_on_tick(ch_async_client_t *c, uint64_t mono_ms);

void ch_async_stats(const ch_async_client_t *c, ch_async_stats_t *out);

/** Pending row count not yet flushed. */
size_t ch_async_pending_rows(const ch_async_client_t *c);

#ifdef __cplusplus
}
#endif

#endif /* CLICKHOUSE_ASYNC_H */
