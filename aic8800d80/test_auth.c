/* SPDX-License-Identifier: GPL-2.0-only */
/* A complete synthetic WPA2 authenticator exchange, including rejection of
 * a forged MIC. All keys/passwords here are public test values. */
#include "includes.h"
#include "common.h"
#include "eloop.h"
#include "crypto/sha1.h"
#include "crypto/aes_wrap.h"
#include "common/wpa_common.h"
#include "auth.h"
#include <assert.h>

static const u8 mac[6]={2,0,0,0,0,1}, bssid[6]={2,0,0,0,0,2};
static const u8 rsn[22]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
struct capture { u8 eapol[512]; size_t n; unsigned sent,keys,complete,failed; };
static int key(void *ctx,bool pairwise,unsigned idx,const uint8_t *p,size_t n)
{ struct capture *c=ctx; (void)p; assert(n==16 && (pairwise || idx==1)); c->keys++; return 0; }
static int send_frame(void *ctx,const uint8_t *dst,const uint8_t *p,size_t n)
{ struct capture *c=ctx; assert(!memcmp(dst,bssid,6) && n<sizeof(c->eapol)); memcpy(c->eapol,p,n); c->n=n; c->sent++; return 0; }
static void complete(void *ctx) { ((struct capture *)ctx)->complete++; }
static void failed(void *ctx) { ((struct capture *)ctx)->failed++; }
int main(void)
{
    assert(!eloop_init()); struct capture c={0};
    struct aic_auth_ops ops={.ctx=&c,.key=key,.send=send_frame,.complete=complete,.failed=failed};
    struct aic_auth *a=aic_auth_create(&ops,mac,bssid,"IEEE","password",rsn,sizeof(rsn)); assert(a);
    aic_auth_associated(a);
    u8 m1[99]={2,3,0,95,2}; WPA_PUT_BE16(m1+5,0x8a); WPA_PUT_BE16(m1+7,16);
    m1[16]=1; memset(m1+17,0x11,32);
    aic_auth_rx(a,bssid,m1,sizeof(m1),0);
    assert(c.sent==1 && c.keys==0 && !c.complete && c.n==121);
    u8 pmk[32]; assert(!pbkdf2_sha1("password",(const u8 *)"IEEE",4,4096,pmk,sizeof(pmk)));
    struct wpa_ptk ptk;
    assert(!wpa_pmk_to_ptk(pmk,32,"Pairwise key expansion",mac,bssid,m1+17,c.eapol+17,
        &ptk,WPA_KEY_MGMT_PSK,WPA_CIPHER_CCMP,NULL,0,0));
    u8 expected[16], m2[512]; memcpy(m2,c.eapol,c.n); memset(m2+81,0,16);
    assert(!wpa_eapol_key_mic(ptk.kck,ptk.kck_len,WPA_KEY_MGMT_PSK,2,m2,c.n,expected));
    assert(!memcmp(expected,c.eapol+81,16));
    u8 plain[48]={0}; memcpy(plain,rsn,sizeof(rsn));
    const u8 kde[8]={221,22,0,15,172,1,1,0}; memcpy(plain+22,kde,8); memset(plain+30,0x33,16);
    plain[46]=221;
    u8 m3[155]={2,3,0,151,2}; WPA_PUT_BE16(m3+5,0x13ca); WPA_PUT_BE16(m3+7,16);
    m3[16]=2; memcpy(m3+17,m1+17,32); WPA_PUT_BE16(m3+97,56);
    assert(!aes_wrap(ptk.kek,ptk.kek_len,6,plain,m3+99));
    assert(!wpa_eapol_key_mic(ptk.kck,ptk.kck_len,WPA_KEY_MGMT_PSK,2,m3,sizeof(m3),m3+81));
    m3[81]^=1; aic_auth_rx(a,bssid,m3,sizeof(m3),0);
    assert(!c.keys && !c.complete && c.sent==1); /* forged key never installed */
    m3[81]^=1; aic_auth_rx(a,bssid,m3,sizeof(m3),0);
    assert(c.keys==2 && c.complete==1 && c.sent==2 && c.n==99 && !c.failed);
    /* Retransmitted M3 must not reinstall the pairwise key (KRACK). */
    aic_auth_rx(a,bssid,m3,sizeof(m3),0); assert(c.keys==2);
    aic_auth_destroy(a); eloop_destroy();
    puts("WPA2 tests passed: full four-way exchange, encrypted GTK, MIC validation and replay/key-reinstall protection.");
    return 0;
}
