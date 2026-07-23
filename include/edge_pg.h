/**
 * @file edge_pg.h
 * @brief Non-blocking Postgres simple-query client over Unix domain socket.
 *
 * Uses poll(2) for connect/read/write (same style as edge_tls_*_poll).
 * No libpq dependency — wire protocol simple query only.
 */
#ifndef EDGE_PG_H
#define EDGE_PG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDGE_PG_PATH_MAX 108 /* matches sockaddr_un.sun_path on Linux */
#define EDGE_PG_NAME_MAX 64
#define EDGE_PG_PASS_MAX 128
#define EDGE_PG_COLS_MAX 32
#define EDGE_PG_ROWS_MAX 256
#define EDGE_PG_CELL_MAX 8192
#define EDGE_PG_ERR_MAX  256

typedef struct {
    char     sock_path[EDGE_PG_PATH_MAX]; /* e.g. /var/run/postgresql/.s.PGSQL.5432 */
    char     database[EDGE_PG_NAME_MAX];
    char     user[EDGE_PG_NAME_MAX];
    char     password[EDGE_PG_PASS_MAX];  /* empty = trust/peer */
    uint32_t timeout_ms;                  /* default 5000 */
} edge_pg_config_t;

typedef struct edge_pg_conn edge_pg_conn_t;

typedef struct {
    int    n_cols;
    int    n_rows;
    char   colnames[EDGE_PG_COLS_MAX][64];
    char  *cells[EDGE_PG_ROWS_MAX][EDGE_PG_COLS_MAX]; /* host_alloc; free via clear */
    char   err[EDGE_PG_ERR_MAX];
    int    ok; /* 1 if CommandComplete without ErrorResponse */
} edge_pg_result_t;

void edge_pg_config_defaults(edge_pg_config_t *c);

/**
 * Connect via Unix domain socket (non-blocking + poll).
 * @return connection or NULL.
 */
edge_pg_conn_t *edge_pg_connect(const edge_pg_config_t *cfg);

void edge_pg_close(edge_pg_conn_t *c);

/**
 * Run one simple Query; fill @p out (caller edge_pg_result_clear).
 * @return 0 ok (check out->ok), -1 transport/protocol error.
 */
int edge_pg_exec(edge_pg_conn_t *c, const char *sql, edge_pg_result_t *out);

void edge_pg_result_clear(edge_pg_result_t *r);

/** Cell accessor; NULL if OOB. */
const char *edge_pg_cell(const edge_pg_result_t *r, int row, int col);

/**
 * Escape string for SQL literal into @p out (single-quote doubled).
 * @return 0 ok, -1 overflow.
 */
int edge_pg_escape_literal(const char *in, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_PG_H */
