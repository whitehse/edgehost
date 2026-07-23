/**
 * @file test_e7_config_xml.c
 * @brief Unit tests for E7 config XML → inventory extract + JSON.
 */

#include "edge_e7_config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *load_file(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "r");
    long sz;
    char *buf;
    size_t n;
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    sz = ftell(fp);
    if (sz < 0 || fseek(fp, 0, SEEK_SET) != 0) {
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

static void test_inventory_sample(void)
{
    size_t len = 0;
    char *xml = load_file("tests/fixtures/e7/calix_e7_running_config_sample.xml",
                          &len);
    edge_e7_cfg_inventory_t *inv;
    int i;
    int found_a = 0, found_b = 0, found_c = 0;

    assert(xml && len > 0);
    inv = (edge_e7_cfg_inventory_t *)calloc(1, sizeof(*inv));
    assert(inv);
    assert(edge_e7_config_extract_inventory(xml, len, inv) == 0);
    printf("  ont_count=%d service_count=%d account_count=%d\n", inv->ont_count,
           inv->service_count, inv->account_count);
    assert(inv->ont_count == 3);
    assert(inv->account_count == 3);
    assert(inv->service_count >= 3);

    for (i = 0; i < inv->ont_count; i++) {
        const edge_e7_cfg_ont_t *o = &inv->onts[i];
        if (strcmp(o->fsan, "CXNK00A1B2C3") == 0) {
            found_a = 1;
            assert(strcmp(o->account, "A-10428") == 0);
            assert(strcmp(o->ont_id, "1/1/3/12") == 0);
            assert(o->port_count >= 1);
            assert(o->ports[0].service_count >= 1);
        }
        if (strcmp(o->fsan, "CXNK00D4E5F6") == 0) {
            found_b = 1;
            assert(strcmp(o->account, "A-10991") == 0);
        }
        if (strcmp(o->fsan, "CXNK00G7H8I9") == 0) {
            found_c = 1;
            assert(strcmp(o->account, "A-11204") == 0);
        }
    }
    assert(found_a && found_b && found_c);

    {
        char sum[256];
        char row[2048];
        assert(edge_e7_cfg_summary_json(inv, sum, sizeof(sum)) > 0);
        assert(strstr(sum, "\"ont_count\":3") != NULL);
        assert(edge_e7_cfg_ont_json(&inv->onts[0], "00:02:5d:d9:21:47",
                                    "2026-01-01T00:00:00Z", row,
                                    sizeof(row)) > 0);
        assert(strstr(row, "A-10428") != NULL || strstr(row, "CXNK") != NULL);
    }

    free(inv);
    free(xml);
    printf("  PASS: inventory sample\n");
}

static void test_xml_to_json(void)
{
    size_t len = 0;
    char *xml = load_file("tests/fixtures/e7/calix_e7_running_config_sample.xml",
                          &len);
    char *json = NULL;
    size_t jlen = 0;
    assert(xml);
    assert(edge_e7_xml_to_json(xml, len, &json, &jlen) == 0);
    assert(json && jlen > 0);
    assert(json[0] == '{');
    assert(strstr(json, "ont") != NULL || strstr(json, "config") != NULL);
    free(json);
    free(xml);
    printf("  PASS: xml_to_json sample\n");
}

/* Calix AXOS-R24 field map (live redacted dump). */
static void test_inventory_axos_fixture(void)
{
    size_t len = 0;
    char *xml =
        load_file("tests/fixtures/e7/calix_axos_running_config_sample.xml", &len);
    edge_e7_cfg_inventory_t *inv;
    int i;
    int found_hsi = 0;
    int found_voice = 0;
    int found_fsan = 0;

    assert(xml && len > 0);
    inv = (edge_e7_cfg_inventory_t *)calloc(1, sizeof(*inv));
    assert(inv);
    assert(edge_e7_config_extract_inventory(xml, len, inv) == 0);
    printf("  axos fixture ont_count=%d service_count=%d account_count=%d\n",
           inv->ont_count, inv->service_count, inv->account_count);
    assert(inv->ont_count == 4);
    assert(inv->account_count >= 3);

    for (i = 0; i < inv->ont_count; i++) {
        const edge_e7_cfg_ont_t *o = &inv->onts[i];
        int p;
        if (strcmp(o->fsan, "CXNKA7158F") == 0) {
            found_fsan = 1;
            assert(strcmp(o->account, "12849401") == 0);
            assert(strcmp(o->model, "803G") == 0 || o->model[0] != '\0');
        }
        if (strcmp(o->fsan, "CXNKDFB757") == 0) {
            /* serial without subscriber-id still listed */
            assert(o->ont_id[0] != '\0');
        }
        for (p = 0; p < o->port_count; p++) {
            int s;
            for (s = 0; s < o->ports[p].service_count; s++) {
                const edge_e7_cfg_service_t *sv = &o->ports[p].services[s];
                if (strcmp(sv->kind, "hsi") == 0) {
                    found_hsi = 1;
                }
                if (strcmp(sv->kind, "voice") == 0) {
                    found_voice = 1;
                }
            }
        }
    }
    assert(found_fsan);
    assert(found_hsi);
    assert(found_voice); /* ont 1454 has Voice policy on ont-ua */

    free(inv);
    free(xml);
    printf("  PASS: AXOS fixture inventory\n");
}

/* Optional full live dump if present (developer machine). */
static void test_inventory_live_dump_optional(void)
{
    size_t len = 0;
    char *xml = load_file("/tmp/e7_config.txt", &len);
    edge_e7_cfg_inventory_t *inv;
    int i;
    int with_fsan = 0;

    if (!xml) {
        printf("  SKIP: live dump /tmp/e7_config.txt not present\n");
        return;
    }
    inv = (edge_e7_cfg_inventory_t *)calloc(1, sizeof(*inv));
    assert(inv);
    assert(edge_e7_config_extract_inventory(xml, len, inv) == 0);
    printf("  live dump ont_count=%d account_count=%d service_count=%d "
           "truncated=%d\n",
           inv->ont_count, inv->account_count, inv->service_count,
           inv->truncated);
    assert(inv->ont_count >= 200);
    assert(inv->account_count >= 200);
    assert(inv->service_count >= 150);
    for (i = 0; i < inv->ont_count; i++) {
        if (strncmp(inv->onts[i].fsan, "CXNK", 4) == 0) {
            with_fsan++;
        }
    }
    assert(with_fsan >= 200);
    free(inv);
    free(xml);
    printf("  PASS: live AXOS dump inventory\n");
}

int main(void)
{
    printf("edgehost e7_config_xml tests\n");
    test_inventory_sample();
    test_xml_to_json();
    test_inventory_axos_fixture();
    test_inventory_live_dump_optional();
    printf("All e7_config_xml tests passed.\n");
    return 0;
}
