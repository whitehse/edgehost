/**
 * Unit tests: XML→JSON + E7 field extract + batch queue (no live ClickHouse).
 */
#include "edge_clickhouse.h"
#include "clickhouse/clickhouse-async.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_xml_to_json_simple(void)
{
    const char *xml =
        "<notification><ont-event><ont-id>1/1/3/12</ont-id>"
        "<oper-state>up</oper-state></ont-event></notification>";
    char out[2048];
    int n = edge_xml_to_json(xml, strlen(xml), out, sizeof(out));
    assert(n > 0);
    assert(strstr(out, "ont-id") != NULL || strstr(out, "ont-event") != NULL);
    assert(strstr(out, "1/1/3/12") != NULL);
    printf("  PASS: xml_to_json simple (%d bytes)\n", n);
}

static void test_extract_ids(void)
{
    const char *xml =
        "<notification><eventTime>2026-07-20T12:00:00Z</eventTime>"
        "<ont-event><ont-id>1/2/3/4</ont-id><pon-id>1/2/3</pon-id>"
        "<oper-state>down</oper-state></ont-event></notification>";
    char ont[64], pon[64], etype[64], sev[32], et[48];
    edge_e7_event_extract_ids(xml, strlen(xml), ont, sizeof(ont), pon,
                              sizeof(pon), etype, sizeof(etype), sev,
                              sizeof(sev), et, sizeof(et));
    assert(strcmp(ont, "1/2/3/4") == 0);
    assert(strcmp(pon, "1/2/3") == 0);
    assert(strcmp(etype, "ont-event") == 0);
    assert(strstr(et, "2026-07-20") != NULL);
    printf("  PASS: extract ids ont=%s pon=%s type=%s\n", ont, pon, etype);
}

static void test_batch_queue_no_network(void)
{
    ch_async_options_t opt;
    ch_async_client_t *c;
    ch_async_stats_t st;

    ch_async_options_defaults(&opt);
    /* Flush threshold high so we only queue */
    opt.flush_max_rows = 100;
    opt.flush_interval_ms = 60000;
    snprintf(opt.host, sizeof(opt.host), "127.0.0.1");
    c = ch_async_create(&opt);
    assert(c);
    assert(ch_async_insert_json_row(c, "edgehost.e7_netconf_events",
                                    "{\"a\":1}", 7) == 0);
    assert(ch_async_insert_json_row(c, "edgehost.e7_netconf_events",
                                    "{\"b\":2}", 7) == 0);
    assert(ch_async_pending_rows(c) == 2);
    ch_async_stats(c, &st);
    assert(st.rows_queued == 2);
    /* destroy will try flush to localhost — may fail; still free */
    ch_async_destroy(c);
    printf("  PASS: batch queue pending=2\n");
}

int main(void)
{
    printf("clickhouse_xml:\n");
    test_xml_to_json_simple();
    test_extract_ids();
    test_batch_queue_no_network();
    printf("all passed\n");
    return 0;
}
