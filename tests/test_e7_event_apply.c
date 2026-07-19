/**
 * PR-3: lab.v1 ONT/PON apply parsers + MAC/identity helpers.
 *
 * Run with WORKING_DIRECTORY = repo root so fixtures resolve.
 */
#include "edge_e7_event_apply.h"
#include "edge_state.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *load_file(const char *path, size_t *out_len)
{
    FILE *fp;
    long sz;
    char *buf;
    size_t n;

    fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "open failed: %s\n", path);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[n] = '\0';
    if (out_len) {
        *out_len = n;
    }
    return buf;
}

static void test_mac_normalize(void)
{
    char out[EDGE_E7_MAC_MAX];

    assert(edge_e7_mac_normalize("00:02:5D:D9:21:47", out, sizeof(out)) == 0);
    assert(strcmp(out, "00:02:5d:d9:21:47") == 0);

    assert(edge_e7_mac_normalize("00-02-5d-d9-21-47", out, sizeof(out)) == 0);
    assert(strcmp(out, "00:02:5d:d9:21:47") == 0);

    assert(edge_e7_mac_normalize("00025DD92147", out, sizeof(out)) == 0);
    assert(strcmp(out, "00:02:5d:d9:21:47") == 0);

    assert(edge_e7_mac_normalize("00:02:5d:d9:21", out, sizeof(out)) != 0);
    assert(edge_e7_mac_normalize("gg:02:5d:d9:21:47", out, sizeof(out)) != 0);
    assert(edge_e7_mac_normalize(NULL, out, sizeof(out)) != 0);
    assert(edge_e7_mac_normalize("00:02:5d:d9:21:47", out, 8) != 0);

    printf("  PASS: mac normalize\n");
}

static void test_key_segs(void)
{
    char out[EDGE_E7_MAC_MAX];
    char aid[EDGE_E7_AID_MAX];

    assert(edge_e7_mac_to_key_seg("00:02:5d:d9:21:47", out, sizeof(out)) == 0);
    assert(strcmp(out, "00-02-5d-d9-21-47") == 0);

    /* Uppercase / hyphen input still yields hyphen key segment. */
    assert(edge_e7_mac_to_key_seg("00-02-5D-D9-21-47", out, sizeof(out)) == 0);
    assert(strcmp(out, "00-02-5d-d9-21-47") == 0);

    assert(edge_e7_aid_to_key_seg("1/1/3/12", aid, sizeof(aid)) == 0);
    assert(strcmp(aid, "1-1-3-12") == 0);

    assert(edge_e7_aid_to_key_seg("1/1/3", aid, sizeof(aid)) == 0);
    assert(strcmp(aid, "1-1-3") == 0);

    assert(edge_e7_aid_to_key_seg("PON/A", aid, sizeof(aid)) == 0);
    assert(strcmp(aid, "pon-a") == 0);

    assert(edge_e7_aid_to_key_seg("", aid, sizeof(aid)) != 0);
    assert(edge_e7_aid_to_key_seg(NULL, aid, sizeof(aid)) != 0);

    printf("  PASS: mac/aid key segments\n");
}

static void test_identity_parse(void)
{
    size_t len = 0;
    char *xml = load_file("tests/fixtures/e7/lab_v1_identity.xml", &len);
    edge_e7_identity_t id;
    const char *inline_xml =
        "<version>1</version><identity><mac>00:02:5D:D9:21:47</mac>"
        "<serial-number>071904926728</serial-number>"
        "<model-name>E7 System</model-name>"
        "<source-ip>192.168.35.13</source-ip></identity>";

    assert(xml != NULL);
    assert(len > 0);

    memset(&id, 0, sizeof(id));
    assert(edge_e7_identity_parse(xml, len, &id) == 0);
    assert(id.identity_ok == 1);
    assert(strcmp(id.mac, "00:02:5d:d9:21:47") == 0);
    assert(strcmp(id.serial, "071904926728") == 0);
    assert(strcmp(id.model, "E7 System") == 0);
    assert(strcmp(id.source_ip, "192.168.35.13") == 0);

    memset(&id, 0, sizeof(id));
    assert(edge_e7_identity_parse(inline_xml, strlen(inline_xml), &id) == 0);
    assert(strcmp(id.mac, "00:02:5d:d9:21:47") == 0);

    assert(edge_e7_identity_parse("<identity></identity>", 20, &id) != 0);
    assert(edge_e7_identity_parse(NULL, 1, &id) != 0);

    free(xml);
    printf("  PASS: identity parse\n");
}

static edge_state_store_t *make_pon_store(void)
{
    edge_state_store_t *st = edge_state_create();
    assert(st);
    assert(edge_state_ns_set_enabled(st, "net.pon", 1) == 0);
    assert(edge_state_ns_enabled(st, "net.pon"));
    return st;
}

static void test_apply_ont_up_down(void)
{
    edge_state_store_t *st = make_pon_store();
    size_t len_up = 0, len_down = 0, n = 0;
    char *up = load_file("tests/fixtures/e7/lab_v1_ont_up.xml", &len_up);
    char *down = load_file("tests/fixtures/e7/lab_v1_ont_down.xml", &len_down);
    char buf[512];
    edge_e7_apply_err_t ae;
    const char *mac = "00:02:5D:D9:21:47";
    const char *key = "e7/00-02-5d-d9-21-47/ont/1-1-3-12";

    assert(up && down);

    ae = edge_e7_event_apply_lab_v1(st, mac, up, len_up);
    assert(ae == EDGE_E7_APPLY_OK);

    assert(edge_state_get(st, "net.pon", key, buf, sizeof(buf), &n) ==
           EDGE_STATE_OK);
    assert(strstr(buf, "\"ont_id\":\"1/1/3/12\"") != NULL);
    assert(strstr(buf, "\"pon_id\":\"1/1/3\"") != NULL);
    assert(strstr(buf, "\"oper_state\":\"up\"") != NULL);
    assert(strstr(buf, "\"mac\":\"00:02:5d:d9:21:47\"") != NULL);
    assert(strstr(buf, "\"source\":\"lab.v1\"") != NULL);
    assert(strstr(buf, "\"event_time\":\"2026-07-19T12:00:00Z\"") != NULL);

    ae = edge_e7_event_apply_lab_v1(st, mac, down, len_down);
    assert(ae == EDGE_E7_APPLY_OK);
    assert(edge_state_get(st, "net.pon", key, buf, sizeof(buf), &n) ==
           EDGE_STATE_OK);
    assert(strstr(buf, "\"oper_state\":\"down\"") != NULL);
    assert(strstr(buf, "\"event_time\":\"2026-07-19T12:05:00Z\"") != NULL);

    free(up);
    free(down);
    edge_state_destroy(st);
    printf("  PASS: apply ont up/down\n");
}

static void test_apply_pon_alarm(void)
{
    edge_state_store_t *st = make_pon_store();
    size_t len = 0, n = 0;
    char *xml = load_file("tests/fixtures/e7/lab_v1_pon_alarm.xml", &len);
    char buf[512];
    edge_e7_apply_err_t ae;
    const char *key = "e7/00-02-5d-d9-21-47/pon/1-1-3";

    assert(xml);
    ae = edge_e7_event_apply_lab_v1(st, "00:02:5d:d9:21:47", xml, len);
    assert(ae == EDGE_E7_APPLY_OK);

    assert(edge_state_get(st, "net.pon", key, buf, sizeof(buf), &n) ==
           EDGE_STATE_OK);
    assert(strstr(buf, "\"pon_id\":\"1/1/3\"") != NULL);
    assert(strstr(buf, "\"alarm\":\"los\"") != NULL);
    assert(strstr(buf, "\"severity\":\"major\"") != NULL);
    assert(strstr(buf, "\"source\":\"lab.v1\"") != NULL);
    assert(strstr(buf, "\"event_time\":\"2026-07-19T12:10:00Z\"") != NULL);

    free(xml);
    edge_state_destroy(st);
    printf("  PASS: apply pon alarm\n");
}

static void test_net_pon_must_be_enabled(void)
{
    edge_state_store_t *st = edge_state_create();
    size_t len = 0;
    char *up = load_file("tests/fixtures/e7/lab_v1_ont_up.xml", &len);
    edge_e7_apply_err_t ae;

    assert(st && up);
    assert(!edge_state_ns_enabled(st, "net.pon"));

    ae = edge_e7_event_apply_lab_v1(st, "00:02:5d:d9:21:47", up, len);
    assert(ae == EDGE_E7_APPLY_STATE_ERR);

    free(up);
    edge_state_destroy(st);
    printf("  PASS: net.pon must be enabled\n");
}

static void test_unknown_and_bad_args(void)
{
    edge_state_store_t *st = make_pon_store();
    const char *junk =
        "<notification xmlns=\"urn:ietf:params:xml:ns:netconf:notification:1.0\">"
        "<eventTime>2026-07-19T12:00:00Z</eventTime>"
        "<other-event xmlns=\"urn:edgehost:lab:e7:1.0\"><x>1</x></other-event>"
        "</notification>";

    assert(edge_e7_event_apply_lab_v1(st, "00:02:5d:d9:21:47", junk,
                                      strlen(junk)) ==
           EDGE_E7_APPLY_UNKNOWN_EVENT);
    assert(edge_e7_event_apply_lab_v1(NULL, "00:02:5d:d9:21:47", junk, 1) ==
           EDGE_E7_APPLY_BAD_ARG);
    assert(edge_e7_event_apply_lab_v1(st, "not-a-mac", junk, strlen(junk)) ==
           EDGE_E7_APPLY_BAD_MAC);

    edge_state_destroy(st);
    printf("  PASS: unknown event / bad args\n");
}

int main(void)
{
    printf("test_e7_event_apply\n");
    test_mac_normalize();
    test_key_segs();
    test_identity_parse();
    test_apply_ont_up_down();
    test_apply_pon_alarm();
    test_net_pon_must_be_enabled();
    test_unknown_and_bad_args();
    printf("All e7_event_apply tests passed\n");
    return 0;
}
