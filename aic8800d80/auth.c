/* SPDX-License-Identifier: GPL-2.0-only */
/* WPA2 authentication is performed by upstream wpa_supplicant 2.11. */
#include "includes.h"
#include "common.h"
#include "eloop.h"
#include "rsn_supp/wpa.h"
#include "crypto/sha1.h"
#include "auth.h"

struct aic_auth {
    struct aic_auth_ops ops;
    struct wpa_sm *sm;
    enum wpa_states state;
    uint8_t bssid[6], rsn[256], ie[256];
    size_t rsn_len, ie_len;
};

int aic_auth_validate(const uint8_t *rsn, size_t n)
{
    struct wpa_ie_data data;
    if (!n || wpa_parse_wpa_ie_rsn(rsn, n, &data) ||
        !(data.pairwise_cipher & WPA_CIPHER_CCMP) ||
        data.group_cipher != WPA_CIPHER_CCMP ||
        !(data.key_mgmt & WPA_KEY_MGMT_PSK) || (data.capabilities & WPA_CAPABILITY_MFPR)) {
        fprintf(stderr, "This build requires WPA2-PSK/CCMP with optional/disabled PMF.\n");
        return -1;
    }
    return 0;
}
static void auth_state(void *ctx, enum wpa_states state)
{
    struct aic_auth *a = ctx;
    a->state = state;
    if (state == WPA_4WAY_HANDSHAKE) puts("WPA2: four-way handshake in progress.");
    if (state == WPA_COMPLETED) a->ops.complete(a->ops.ctx);
}
static enum wpa_states auth_get_state(void *ctx) { return ((struct aic_auth *)ctx)->state; }
static void auth_deauth(void *ctx, u16 reason)
{
    struct aic_auth *a = ctx;
    fprintf(stderr, "WPA2 authentication failed (reason %u).\n", reason);
    a->ops.failed(a->ops.ctx);
}
static int auth_key(void *ctx, int link, enum wpa_alg alg, const u8 *addr,
                    int idx, int tx, const u8 *seq, size_t seq_len,
                    const u8 *key, size_t n, enum key_flag flags)
{
    (void)link; (void)tx; (void)seq; (void)seq_len; (void)flags;
    struct aic_auth *a = ctx;
    if (alg == WPA_ALG_NONE) return 0; /* reset/remove station clears hardware keys */
    if (alg != WPA_ALG_CCMP || n != 16 || idx < 0 || idx > 3) return -1;
    return a->ops.key(a->ops.ctx, addr && !(addr[0]&1), idx, key, n);
}
static void *auth_network(void *ctx) { return ctx; }
static int auth_bssid(void *ctx, u8 *bssid) { memcpy(bssid, ((struct aic_auth *)ctx)->bssid, 6); return 0; }
static int auth_send(void *ctx, const u8 *dest, u16 proto, const u8 *buf, size_t n)
{
    struct aic_auth *a = ctx;
    if (proto != ETH_P_EAPOL) return -1;
    return a->ops.send(a->ops.ctx, dest, buf, n);
}
static int auth_beacon(void *ctx)
{
    struct aic_auth *a = ctx;
    return wpa_sm_set_ap_rsn_ie(a->sm, a->rsn, a->rsn_len);
}
static void auth_cancel(void *ctx) { (void)ctx; }
static u8 *auth_alloc(void *ctx, u8 type, const void *data, u16 n, size_t *len, void **pos)
{
    (void)ctx;
    *len = 4+n;
    u8 *p = os_zalloc(*len);
    if (!p) return NULL;
    p[0] = 2; p[1] = type; WPA_PUT_BE16(p+2,n);
    if (data) memcpy(p+4,data,n);
    *pos = p+4;
    return p;
}
static int auth_add_pmkid(void *ctx, void *network, const u8 *bssid, const u8 *pmkid,
                          const u8 *cache, const u8 *pmk, size_t len, u32 life, u8 threshold, int akm)
{
    (void)ctx; (void)network; (void)bssid; (void)pmkid; (void)cache;
    (void)pmk; (void)len; (void)life; (void)threshold; (void)akm;
    return 0;
}
static int auth_remove_pmkid(void *ctx, void *network, const u8 *bssid, const u8 *pmkid, const u8 *cache)
{ (void)ctx; (void)network; (void)bssid; (void)pmkid; (void)cache; return 0; }
static int auth_protection(void *ctx, const u8 *addr, int protect, int type)
{ (void)ctx; (void)addr; (void)protect; (void)type; return 0; }

struct aic_auth *aic_auth_create(const struct aic_auth_ops *ops, const uint8_t *mac,
                                const uint8_t *bssid, const char *ssid, const char *password,
                                const uint8_t *rsn, size_t n)
{
    if (aic_auth_validate(rsn,n)) return NULL;
    struct aic_auth *a = os_zalloc(sizeof(*a));
    struct wpa_sm_ctx *ctx = os_zalloc(sizeof(*ctx));
    if (!a || !ctx) { os_free(a); os_free(ctx); return NULL; }
    a->ops = *ops;
    memcpy(a->bssid,bssid,6); memcpy(a->rsn,rsn,n); a->rsn_len=n;
    *ctx = (struct wpa_sm_ctx){.ctx=a, .set_state=auth_state, .get_state=auth_get_state,
        .deauthenticate=auth_deauth, .set_key=auth_key, .get_network_ctx=auth_network,
        .get_bssid=auth_bssid, .ether_send=auth_send, .get_beacon_ie=auth_beacon,
        .cancel_auth_timeout=auth_cancel, .alloc_eapol=auth_alloc, .add_pmkid=auth_add_pmkid,
        .remove_pmkid=auth_remove_pmkid, .mlme_setprotection=auth_protection};
    a->sm = wpa_sm_init(ctx);
    if (!a->sm) { os_free(ctx); os_free(a); return NULL; }
    struct rsn_supp_config conf = {.network_ctx=a, .allowed_pairwise_cipher=WPA_CIPHER_CCMP,
        .ssid=(const u8 *)ssid, .ssid_len=strlen(ssid)};
    wpa_sm_set_config(a->sm,&conf);
    wpa_sm_set_own_addr(a->sm,mac);
    wpa_sm_set_param(a->sm,WPA_PARAM_PROTO,WPA_PROTO_RSN);
    wpa_sm_set_param(a->sm,WPA_PARAM_KEY_MGMT,WPA_KEY_MGMT_PSK);
    wpa_sm_set_param(a->sm,WPA_PARAM_PAIRWISE,WPA_CIPHER_CCMP);
    wpa_sm_set_param(a->sm,WPA_PARAM_GROUP,WPA_CIPHER_CCMP);
    wpa_sm_set_param(a->sm,WPA_PARAM_RSN_ENABLED,1);
    uint8_t pmk[32];
    size_t plen = strlen(password);
    int invalid = plen==64 ? hexstr2bin(password,pmk,32) :
        (plen<8 || plen>63 ? -1 : pbkdf2_sha1(password,(const u8 *)ssid,strlen(ssid),4096,pmk,32));
    if (invalid) {
        fprintf(stderr, "WPA2 password must be 8-63 bytes or a 64-digit hexadecimal PSK.\n");
        forced_memzero(pmk,sizeof(pmk)); aic_auth_destroy(a); return NULL;
    }
    wpa_sm_set_pmk(a->sm,pmk,sizeof(pmk),NULL,NULL);
    forced_memzero(pmk,sizeof(pmk));
    a->ie_len=sizeof(a->ie);
    if (wpa_sm_set_assoc_wpa_ie_default(a->sm,a->ie,&a->ie_len) || auth_beacon(a)) {
        aic_auth_destroy(a); return NULL;
    }
    return a;
}
const uint8_t *aic_auth_ie(struct aic_auth *a, size_t *n) { *n=a->ie_len; return a->ie; }
void aic_auth_associated(struct aic_auth *a) { a->state=WPA_ASSOCIATED; wpa_sm_notify_assoc(a->sm,a->bssid); }
int aic_auth_rx(struct aic_auth *a, const uint8_t *src, const uint8_t *p, size_t n, int enc)
{ return wpa_sm_rx_eapol(a->sm,src,p,n,enc?FRAME_ENCRYPTED:FRAME_NOT_ENCRYPTED); }
void aic_auth_destroy(struct aic_auth *a) { if (a) { wpa_sm_deinit(a->sm); os_free(a); } }
