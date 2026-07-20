#include "edge_e7_event_apply.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

static void hmac_hex(const char *secret, const char *host_key, char *out, size_t out_sz)
{
    unsigned char md[20];
    unsigned int md_len = 0;
    size_t i;
    assert(HMAC(EVP_sha1(), secret, (int)strlen(secret),
                (const unsigned char *)host_key, strlen(host_key), md, &md_len));
    assert(md_len == 20 && out_sz >= 41);
    for (i = 0; i < 20; i++) {
        static const char xd[] = "0123456789abcdef";
        out[i * 2] = xd[md[i] >> 4];
        out[i * 2 + 1] = xd[md[i] & 0xf];
    }
    out[40] = '\0';
}

int main(void)
{
    edge_e7_identity_t id;
    static const char partial[] =
        "MSG-ID: DEVICE-CONN-INFO\r\n"
        "MSG-VER: V1\r\n"
        "DEVICE-ID: pe1.lab\r\n";
    static const char plain_ssh[] =
        "MSG-ID: DEVICE-CONN-INFO\r\n"
        "MSG-VER: V1\r\n"
        "DEVICE-ID: pe1.lab\r\n"
        "SSH-2.0-OpenSSH_8.9\r\n";
    static const char plain_blank[] =
        "MSG-ID: DEVICE-CONN-INFO\r\n"
        "MSG-VER: V1\r\n"
        "DEVICE-ID: pe2.lab\r\n"
        "\r\n";
    char host_key[] = "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQC testkey";
    char hex[48];
    char with_hmac[512];
    char delayed_hk[512];
    int r;

    assert(edge_e7_junos_identity_looks_like(partial, strlen(partial)));

    /* DEVICE-ID only (no terminator): wait — HOST-KEY may still arrive. */
    r = edge_e7_junos_identity_parse(partial, strlen(partial), &id);
    assert(r == 1);

    /* Complete without secret when SSH banner follows. */
    r = edge_e7_junos_identity_parse(plain_ssh, strlen(plain_ssh), &id);
    assert(r == 0 && id.identity_ok);
    assert(strcmp(id.device_id, "pe1.lab") == 0);
    assert(strcmp(id.vendor, "junos") == 0);
    assert(!id.has_hmac);
    assert(id.consumed < strlen(plain_ssh));
    assert(memcmp(plain_ssh + id.consumed, "SSH-", 4) == 0);

    /* Complete without secret on blank-line terminator. */
    r = edge_e7_junos_identity_parse(plain_blank, strlen(plain_blank), &id);
    assert(r == 0 && id.identity_ok);
    assert(strcmp(id.device_id, "pe2.lab") == 0);

    hmac_hex("s3cret", host_key, hex, sizeof(hex));
    snprintf(with_hmac, sizeof(with_hmac),
             "MSG-ID: DEVICE-CONN-INFO\r\n"
             "MSG-VER: V1\r\n"
             "DEVICE-ID: router1\r\n"
             "HOST-KEY: %s\r\n"
             "HMAC:%s\r\n"
             "SSH-2.0-OpenSSH_8.9\r\n",
             host_key, hex);
    r = edge_e7_junos_identity_parse(with_hmac, strlen(with_hmac), &id);
    assert(r == 0 && id.identity_ok && id.has_host_key && id.has_hmac);
    assert(strcmp(id.device_id, "router1") == 0);
    assert(edge_e7_junos_hmac_verify(id.host_key, id.hmac, "s3cret") == 0);
    assert(edge_e7_junos_hmac_verify(id.host_key, id.hmac, "wrong") != 0);
    /* SSH banner not consumed */
    assert(id.consumed < strlen(with_hmac));
    assert(memcmp(with_hmac + id.consumed, "SSH-", 4) == 0);

    /* Split TCP: DEVICE-ID first packet must not finalize before HOST-KEY. */
    snprintf(delayed_hk, sizeof(delayed_hk),
             "%sHOST-KEY: %s\r\nHMAC:%s\r\n", partial, host_key, hex);
    r = edge_e7_junos_identity_parse(partial, strlen(partial), &id);
    assert(r == 1);
    r = edge_e7_junos_identity_parse(delayed_hk, strlen(delayed_hk), &id);
    assert(r == 0 && id.identity_ok && id.has_hmac);
    assert(edge_e7_junos_hmac_verify(id.host_key, id.hmac, "s3cret") == 0);

    {
        char seg[64];
        assert(edge_e7_device_key_seg("PE1.Lab/01", seg, sizeof(seg)) == 0);
        assert(strcmp(seg, "pe1.lab-01") == 0);
    }

    printf("  PASS: Junos DEVICE-CONN-INFO parse + HMAC-SHA1 verify\n");
    return 0;
}
