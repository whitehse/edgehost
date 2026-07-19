/**
 * @file sim_main.c
 * @brief Class-A simulation host for edgehost (P1.5 / ADR-011).
 *
 * Uses libsim (clock, net, uring) and host_alloc — no real kernel sockets.
 */

#include "edge_sim_main.h"

#include "edge_config.h"
#include "edge_http1_serve.h"
#include "edge_metrics.h"
#include "edgecore.h"
#include "host_alloc.h"

#include "sim.h"

#include <stdio.h>
#include <string.h>

static void drain_cq(sim_uring_t *r)
{
    sim_cqe_t *cqe;
    while (sim_uring_peek_cqe(r, &cqe)) {
        sim_uring_cqe_seen(r, cqe);
    }
}

/**
 * Mini class-A path: listen → connect → uring accept → send request →
 * uring recv → HTTP serve → uring send response.
 */
static void sim_http_exchange(const uint8_t *req, size_t req_len,
                              edge_metrics_t *metrics)
{
    sim_clock_t *clk = NULL;
    sim_net_t *net = NULL;
    sim_uring_t *ring = NULL;
    uint64_t listen_fd = 0;
    uint64_t client_fd = 0;
    uint64_t accepted = 0;
    sim_sqe_t *sqe;
    sim_cqe_t *cqe;
    edge_http1_serve_t *srv = NULL;
    uint8_t rbuf[2048];
    char resp[2048];
    size_t resp_len = 0;
    int pr;
    int n;

    if (!req || req_len == 0) {
        return;
    }

    clk = sim_clock_create();
    net = sim_net_create();
    if (!clk || !net) {
        goto out;
    }
    ring = sim_uring_create(net, clk);
    if (!ring) {
        goto out;
    }

    listen_fd = sim_net_socket(net);
    client_fd = sim_net_socket(net);
    if (!listen_fd || !client_fd) {
        goto out;
    }
    if (sim_net_listen(net, listen_fd) != 0) {
        goto out;
    }
    if (sim_net_connect(net, client_fd, listen_fd) != 0) {
        goto out;
    }

    /* Accept via sim_uring */
    sqe = sim_uring_get_sqe(ring);
    if (!sqe) {
        goto out;
    }
    sim_sqe_prep_accept(sqe, listen_fd);
    sim_sqe_set_user_data(sqe, 1);
    if (sim_uring_submit(ring) < 1) {
        goto out;
    }
    (void)sim_uring_progress(ring);
    if (sim_uring_peek_cqe(ring, &cqe) != 1 || cqe->res <= 0) {
        drain_cq(ring);
        goto out;
    }
    accepted = (uint64_t)(uint32_t)cqe->res;
    sim_uring_cqe_seen(ring, cqe);

    /* Client sends request bytes (may be truncated adversarial). */
    n = sim_net_send(net, client_fd, req, req_len);
    if (n < 0) {
        n = 0;
    }
    if (metrics && n > 0) {
        /* bytes counted again on serve_feed from server side */
    }

    /* Server recv via uring */
    memset(rbuf, 0, sizeof(rbuf));
    sqe = sim_uring_get_sqe(ring);
    if (!sqe) {
        goto out;
    }
    sim_sqe_prep_recv(sqe, accepted, rbuf, sizeof(rbuf));
    sim_sqe_set_user_data(sqe, 2);
    if (sim_uring_submit(ring) < 1) {
        goto out;
    }
    (void)sim_uring_progress(ring);
    if (sim_uring_peek_cqe(ring, &cqe) != 1) {
        drain_cq(ring);
        goto out;
    }
    n = cqe->res;
    sim_uring_cqe_seen(ring, cqe);
    if (n <= 0) {
        goto out;
    }

    srv = edge_http1_serve_create();
    if (!srv) {
        goto out;
    }
    pr = edge_http1_serve_feed(srv, rbuf, (size_t)n, metrics, resp,
                               sizeof(resp), &resp_len);
    if (pr == 0) {
        /* incomplete headers — inject CRLF terminator once to settle */
        static const uint8_t tail[] = "\r\n\r\n";
        pr = edge_http1_serve_feed(srv, tail, sizeof(tail) - 1, metrics, resp,
                                   sizeof(resp), &resp_len);
    }
    if (pr == 1 && resp_len > 0) {
        sqe = sim_uring_get_sqe(ring);
        if (sqe) {
            sim_sqe_prep_send(sqe, accepted, resp, resp_len);
            sim_sqe_set_user_data(sqe, 3);
            (void)sim_uring_submit(ring);
            (void)sim_uring_progress(ring);
            if (sim_uring_peek_cqe(ring, &cqe)) {
                if (metrics && cqe->res > 0) {
                    metrics->bytes_out += (uint64_t)cqe->res;
                }
                sim_uring_cqe_seen(ring, cqe);
            }
        }
    }

    drain_cq(ring);

out:
    edge_http1_serve_destroy(srv);
    if (ring) {
        sim_uring_destroy(ring);
    }
    if (net) {
        sim_net_destroy(net);
    }
    if (clk) {
        sim_clock_destroy(clk);
    }
}

static void edgecore_alloc_fuzz(sim_fuzz_cursor_t *cur)
{
    edgecore_t *core;
    edge_event_t ev;
    uint32_t id;
    size_t want;
    int ok = 1;
    uint8_t b;
    void *p;

    core = edgecore_create();
    if (!core) {
        return;
    }

    b = sim_fuzz_u8(cur, &ok);
    if (!ok) {
        edgecore_destroy(core);
        return;
    }
    want = 16u + (size_t)(b % 64u);

    id = edgecore_request_alloc(core, EDGE_BUF_GENERIC, want);
    if (id != 0 && edgecore_next_event(core, &ev) == 1 &&
        ev.type == EDGE_EVENT_NEED_ALLOC) {
        p = host_alloc(ev.size);
        if (p) {
            (void)edgecore_provide_buffer(core, id, p, ev.size);
            (void)edgecore_detach_buffer(core, id, &p, NULL);
            host_free(p);
        }
    }

    /* config apply with mutated port */
    {
        edge_config_t cfg;
        edge_config_defaults(&cfg);
        b = sim_fuzz_u8(cur, &ok);
        if (ok && b != 0) {
            cfg.listen_port = (uint16_t)(1u + (b % 200u));
        }
        (void)edgecore_apply_config(core, &cfg);
        while (edgecore_next_event(core, &ev) == 1) {
            /* drain */
        }
    }

    edgecore_destroy(core);
}

int edge_sim_drive(const uint8_t *data, size_t size)
{
    sim_fuzz_cursor_t cur;
    edge_metrics_t metrics;
    uint8_t http_buf[1024];
    size_t http_len = 0;
    int ok = 1;
    uint8_t mode;

    /* 1) Full class-A libsim opcode driver (always, even empty). */
    (void)sim_fuzz_drive_a(data, size);

    edge_metrics_init(&metrics);
    host_alloc_reset_stats();
    sim_fuzz_cursor_init(&cur, data, size);

    /* 2) edgecore memory + config path */
    edgecore_alloc_fuzz(&cur);

    /* 3) Build an HTTP-ish payload: prefer remaining fuzz bytes as request;
     *    if empty, use a valid health GET for coverage. */
    if (sim_fuzz_remaining(&cur) > 0) {
        (void)sim_fuzz_bytes(&cur, http_buf, sizeof(http_buf) - 1, &http_len);
    }
    if (http_len == 0) {
        static const char health[] =
            "GET /health HTTP/1.1\r\nHost: t\r\n\r\n";
        http_len = sizeof(health) - 1;
        memcpy(http_buf, health, http_len);
    } else {
        /* Optionally force a complete GET wrapper when high bit set. */
        mode = sim_fuzz_u8(&cur, &ok);
        if (ok && (mode & 0x01) && http_len < 200) {
            char wrapped[512];
            int n = snprintf(wrapped, sizeof(wrapped),
                             "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
            if (n > 0 && (size_t)n < sizeof(wrapped)) {
                http_len = (size_t)n;
                memcpy(http_buf, wrapped, http_len);
            }
        }
    }

    /* 4) Direct serve (no sim) — pure SM coverage */
    {
        edge_http1_serve_t *s = edge_http1_serve_create();
        char resp[2048];
        size_t rlen = 0;
        if (s) {
            (void)edge_http1_serve_feed(s, http_buf, http_len, &metrics, resp,
                                        sizeof(resp), &rlen);
            edge_http1_serve_destroy(s);
        }
    }

    /* 5) sim_net + sim_uring exchange with same payload */
    sim_http_exchange(http_buf, http_len, &metrics);

    (void)ok;
    return 0;
}
