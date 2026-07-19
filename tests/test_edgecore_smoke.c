/**
 * P1.1: edgecore create / next_event smoke.
 */
#include "edgecore.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_create_empty_queue(void)
{
    edgecore_t *core = edgecore_create();
    edge_event_t ev;

    assert(core);
    assert(edgecore_event_count(core) == 0);
    assert(!edgecore_has_pending_events(core));
    assert(edgecore_dropped_count(core) == 0);

    memset(&ev, 0xAA, sizeof(ev));
    assert(edgecore_next_event(core, &ev) == 0);
    assert(ev.type == EDGE_EVENT_NONE);

    edgecore_destroy(core);
    printf("  PASS: create + empty next_event\n");
}

static void test_create_with_config(void)
{
    edgecore_config_t cfg = edgecore_default_config();
    edgecore_t *core;
    edge_event_t ev;

    cfg.event_queue_size = 8;
    core = edgecore_create_with_config(&cfg);
    assert(core);
    assert(edgecore_next_event(core, &ev) == 0);
    assert(ev.type == EDGE_EVENT_NONE);
    edgecore_destroy(core);
    printf("  PASS: create_with_config\n");
}

static void test_null_safety(void)
{
    edgecore_t *core;
    edge_event_t ev;

    edgecore_destroy(NULL);
    assert(edgecore_next_event(NULL, &ev) == -1);

    core = edgecore_create();
    assert(core);
    assert(edgecore_next_event(core, NULL) == -1);
    edgecore_destroy(core);

    assert(edgecore_event_count(NULL) == 0);
    assert(edgecore_has_pending_events(NULL) == 0);
    assert(edgecore_dropped_count(NULL) == 0);
    printf("  PASS: null safety\n");
}

static void test_event_type_names(void)
{
    assert(strcmp(edgecore_event_type_name(EDGE_EVENT_NONE), "NONE") == 0);
    assert(strcmp(edgecore_event_type_name(EDGE_EVENT_NEED_ALLOC),
                  "NEED_ALLOC") == 0);
    assert(strcmp(edgecore_event_type_name(EDGE_EVENT_QUEUE_OVERFLOW),
                  "QUEUE_OVERFLOW") == 0);
    assert(strcmp(edgecore_event_type_name(EDGE_EVENT_OUTBOUND_DONE),
                  "OUTBOUND_DONE") == 0);
    assert(strcmp(edgecore_event_type_name((edge_event_type_t)999),
                  "UNKNOWN") == 0);
    printf("  PASS: event type names\n");
}

int main(void)
{
    printf("edgecore_smoke:\n");
    test_create_empty_queue();
    test_create_with_config();
    test_null_safety();
    test_event_type_names();
    printf("all passed\n");
    return 0;
}
