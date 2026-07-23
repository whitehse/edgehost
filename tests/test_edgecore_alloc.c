/**
 * P1.2: NEED_ALLOC / NEED_REALLOC + host_alloc provide path.
 */
#include "edgecore.h"
#include "host_alloc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void drain_none(edgecore_t *core)
{
    edge_event_t ev;
    assert(edgecore_next_event(core, &ev) == 0);
    assert(ev.type == EDGE_EVENT_NONE);
}

static void test_need_alloc_roundtrip(void)
{
    edgecore_t *core = edgecore_create();
    edge_event_t ev;
    uint32_t id;
    void *p;
    void *out = NULL;
    size_t out_sz = 0;

    host_alloc_reset_stats();
    assert(core);

    id = edgecore_request_alloc(core, EDGE_BUF_GENERIC, 1024);
    assert(id != 0);
    assert(edgecore_buffer_count(core) == 0); /* pending, not ready */
    assert(edgecore_buffer_ptr(core, id) == NULL);

    assert(edgecore_next_event(core, &ev) == 1);
    assert(ev.type == EDGE_EVENT_NEED_ALLOC);
    assert(ev.alloc_id == id);
    assert(ev.buf_kind == EDGE_BUF_GENERIC);
    assert(ev.size == 1024);
    assert(ev.old_ptr == NULL);
    assert(strcmp(edgecore_event_type_name(ev.type), "NEED_ALLOC") == 0);
    assert(strcmp(edgecore_buf_kind_name(ev.buf_kind), "GENERIC") == 0);

    p = host_alloc(ev.size);
    assert(p);
    assert(host_alloc_count() == 1);

    assert(edgecore_provide_buffer(core, id, p, 1024) == 0);
    assert(edgecore_buffer_ptr(core, id) == p);
    assert(edgecore_buffer_size(core, id) == 1024);
    assert(edgecore_buffer_count(core) == 1);
    drain_none(core);

    /* too-small provide rejected */
    {
        uint32_t id2 = edgecore_request_alloc(core, EDGE_BUF_CONN_RX, 64);
        edge_event_t ev2;
        char tiny[8];
        assert(id2 != 0);
        assert(edgecore_next_event(core, &ev2) == 1);
        assert(edgecore_provide_buffer(core, id2, tiny, 8) == -1);
        assert(edgecore_provide_buffer(core, id2, tiny, 64) == 0);
        assert(edgecore_detach_buffer(core, id2, NULL, NULL) == 0);
        /* tiny is stack — do not host_free */
    }

    assert(edgecore_detach_buffer(core, id, &out, &out_sz) == 0);
    assert(out == p);
    assert(out_sz == 1024);
    assert(edgecore_buffer_count(core) == 0);
    host_free(out);
    assert(host_free_count() == 1);

    edgecore_destroy(core);
    printf("  PASS: NEED_ALLOC roundtrip\n");
}

static void test_need_realloc_roundtrip(void)
{
    edgecore_t *core = edgecore_create();
    edge_event_t ev;
    uint32_t id;
    void *p;
    void *p2;
    void *out = NULL;

    host_alloc_reset_stats();
    assert(core);

    id = edgecore_request_alloc(core, EDGE_BUF_HTTP_BODY, 128);
    assert(id != 0);
    assert(edgecore_next_event(core, &ev) == 1);
    p = host_alloc(ev.size);
    assert(edgecore_provide_buffer(core, id, p, 128) == 0);
    memcpy(p, "hello", 5);

    assert(edgecore_request_realloc(core, id, 512) == 0);
    assert(edgecore_buffer_ptr(core, id) == NULL); /* pending during grow */

    assert(edgecore_next_event(core, &ev) == 1);
    assert(ev.type == EDGE_EVENT_NEED_REALLOC);
    assert(ev.alloc_id == id);
    assert(ev.size == 512);
    assert(ev.old_ptr == p);
    assert(ev.old_size == 128);

    p2 = host_realloc(ev.old_ptr, ev.size);
    assert(p2);
    assert(host_realloc_count() == 1);
    assert(edgecore_provide_buffer(core, id, p2, 512) == 0);
    assert(edgecore_buffer_ptr(core, id) == p2);
    assert(edgecore_buffer_size(core, id) == 512);
    assert(memcmp(p2, "hello", 5) == 0);

    assert(edgecore_detach_buffer(core, id, &out, NULL) == 0);
    host_free(out);
    edgecore_destroy(core);
    printf("  PASS: NEED_REALLOC roundtrip\n");
}

static void test_invalid_ops(void)
{
    edgecore_t *core = edgecore_create();
    edge_event_t ev;
    char buf[32];

    assert(edgecore_request_alloc(NULL, EDGE_BUF_GENERIC, 16) == 0);
    assert(edgecore_request_alloc(core, EDGE_BUF_NONE, 16) == 0);
    assert(edgecore_request_alloc(core, EDGE_BUF_GENERIC, 0) == 0);
    assert(edgecore_request_realloc(core, 999, 64) == -1);
    assert(edgecore_provide_buffer(core, 1, buf, 32) == -1);
    assert(edgecore_detach_buffer(core, 1, NULL, NULL) == -1);
    assert(edgecore_buffer_ptr(core, 0) == NULL);
    assert(edgecore_buffer_size(NULL, 1) == 0);

    /* realloc on pending (not ready) fails */
    {
        uint32_t id = edgecore_request_alloc(core, EDGE_BUF_STATE, 16);
        assert(id != 0);
        assert(edgecore_request_realloc(core, id, 32) == -1);
        assert(edgecore_next_event(core, &ev) == 1);
        assert(edgecore_provide_buffer(core, id, buf, 16) == 0);
        assert(edgecore_detach_buffer(core, id, NULL, NULL) == 0);
    }

    edgecore_destroy(core);
    printf("  PASS: invalid ops\n");
}

static void test_max_bufs(void)
{
    edgecore_t *core = edgecore_create();
    uint32_t ids[EDGECORE_MAX_BUFS];
    edge_event_t ev;
    size_t i;
    char storage[EDGECORE_MAX_BUFS][8];

    for (i = 0; i < EDGECORE_MAX_BUFS; i++) {
        ids[i] = edgecore_request_alloc(core, EDGE_BUF_GENERIC, 8);
        assert(ids[i] != 0);
        assert(edgecore_next_event(core, &ev) == 1);
        assert(edgecore_provide_buffer(core, ids[i], storage[i], 8) == 0);
    }
    assert(edgecore_buffer_count(core) == EDGECORE_MAX_BUFS);
    assert(edgecore_request_alloc(core, EDGE_BUF_GENERIC, 8) == 0);

    for (i = 0; i < EDGECORE_MAX_BUFS; i++) {
        assert(edgecore_detach_buffer(core, ids[i], NULL, NULL) == 0);
    }
    assert(edgecore_request_alloc(core, EDGE_BUF_GENERIC, 8) != 0);
    assert(edgecore_next_event(core, &ev) == 1);
    /* leave pending; destroy does not free host mem (none heap here) */
    edgecore_destroy(core);
    printf("  PASS: max buf slots\n");
}

static void test_host_alloc_byte_tracking(void)
{
    void *a;
    void *b;
    void *c;

    host_alloc_reset_stats();
    a = host_alloc(100);
    assert(a);
    assert(host_bytes_outstanding() == 100);
    assert(host_bytes_outstanding_kind(EDGE_MEM_EDGECORE) == 100);
    assert(host_bytes_peak() == 100);

    b = host_alloc_kind(EDGE_MEM_HTTP, 50);
    assert(b);
    assert(host_bytes_outstanding() == 150);
    assert(host_bytes_outstanding_kind(EDGE_MEM_HTTP) == 50);
    assert(host_bytes_peak() == 150);

    c = host_realloc(a, 200);
    assert(c);
    assert(host_bytes_outstanding() == 250);
    assert(host_bytes_outstanding_kind(EDGE_MEM_EDGECORE) == 200);
    assert(host_bytes_peak() == 250);

    host_free(c);
    assert(host_bytes_outstanding() == 50);
    assert(host_bytes_outstanding_kind(EDGE_MEM_EDGECORE) == 0);
    host_free(b);
    assert(host_bytes_outstanding() == 0);
    assert(host_free_count() == 2);
    assert(host_alloc_count() == 2);
    assert(host_realloc_count() == 1);

    /* kind names stable for JSON */
    assert(strcmp(edge_mem_kind_name(EDGE_MEM_E7), "e7") == 0);
    assert(strcmp(edge_mem_kind_name(EDGE_MEM_WS), "ws") == 0);

    {
        char buf[1024];
        int n = host_mem_format_json(buf, sizeof(buf));
        assert(n > 0);
        assert(strstr(buf, "\"host_alloc\"") != NULL);
        assert(strstr(buf, "\"by_kind\"") != NULL);
        assert(strstr(buf, "\"edgecore\"") != NULL);
    }

    printf("  PASS: host_alloc byte tracking + kinds\n");
}

int main(void)
{
    printf("edgecore_alloc:\n");
    test_need_alloc_roundtrip();
    test_need_realloc_roundtrip();
    test_invalid_ops();
    test_max_bufs();
    test_host_alloc_byte_tracking();
    printf("all passed\n");
    return 0;
}
