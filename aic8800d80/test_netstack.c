/* SPDX-License-Identifier: GPL-2.0-only */
#include "netstack.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const uint8_t mac[6]={2,0,0,0,0,1}, gwmac[6]={2,0,0,0,0,2};
static const uint8_t ip[4]={192,168,1,3}, gateway[4]={192,168,1,1};
struct capture { uint8_t frame[32][1514]; size_t length[32]; unsigned sent,leases,failures,ips; };
static void put16(uint8_t *p,unsigned v) { p[0]=v>>8; p[1]=v; }
static int send_frame(void *ctx,const uint8_t *p,size_t n)
{ struct capture *c=ctx; assert(c->sent<32 && n<=1514); memcpy(c->frame[c->sent],p,n); c->length[c->sent++]=n; return 0; }
static int leased(void *ctx,const struct aic_lease *l)
{ struct capture *c=ctx; assert(!memcmp(l->ip,ip,4) && !memcmp(l->gateway,gateway,4)); c->leases++; return 0; }
static void fail(void *ctx) { ((struct capture *)ctx)->failures++; }
static void receive_ip(void *ctx,const uint8_t *p,size_t n) { (void)p; (void)n; ((struct capture *)ctx)->ips++; }
static size_t ipv4_udp(uint8_t *eth,unsigned source,unsigned dest,const uint8_t *p,size_t n)
{
    memset(eth,0,1514); memcpy(eth,mac,6); memcpy(eth+6,gwmac,6); eth[12]=8;
    uint8_t *h=eth+14; h[0]=0x45; put16(h+2,28+n); h[8]=64; h[9]=17;
    memcpy(h+12,gateway,4); memcpy(h+16,ip,4); put16(h+10,aic_checksum(h,20));
    put16(h+20,source); put16(h+22,dest); put16(h+24,8+n); memcpy(h+28,p,n);
    return 42+n;
}
static size_t dhcp(uint8_t *eth,unsigned type,const uint8_t *xid)
{
    uint8_t p[300]={2,1,6}; memcpy(p+4,xid,4); memcpy(p+16,ip,4); memcpy(p+28,mac,6);
    memcpy(p+236,"\x63\x82\x53\x63",4);
    const uint8_t opts[]={53,1,0,1,4,255,255,255,0,3,4,192,168,1,1,6,4,192,168,1,1,
        54,4,192,168,1,1,51,4,0,1,0x51,0x80,255};
    memcpy(p+240,opts,sizeof(opts)); p[242]=type;
    return ipv4_udp(eth,67,68,p,sizeof(p));
}
int main(void)
{
    struct capture c={0}; struct aic_network_ops ops={.ctx=&c,.send=send_frame,.lease=leased,.failure=fail,.ip=receive_ip};
    struct aic_network *s=aic_network_create(&ops,mac); assert(s);
    aic_network_tick(s,100); assert(c.sent==1);
    uint8_t xid[4]; memcpy(xid,c.frame[0]+46,4);
    uint8_t frame[1514]; size_t n=dhcp(frame,2,xid);
    frame[46]^=1; aic_network_rx(s,frame,n); assert(c.sent==1 && !c.leases);
    frame[46]^=1; aic_network_rx(s,frame,n); assert(c.sent==2 && !c.leases);
    n=dhcp(frame,5,xid); aic_network_rx(s,frame,n); assert(c.leases==1 && !c.failures && !aic_network_ready(s));
    /* Gateway resolution must flush the queued DNS packet with the correct
     * Ethernet destination, rather than sending IP data as a broadcast. */
    uint8_t arp[42]={0}; memcpy(arp,mac,6); memcpy(arp+6,gwmac,6); put16(arp+12,0x806);
    put16(arp+14,1); put16(arp+16,0x800); arp[18]=6; arp[19]=4; put16(arp+20,2);
    memcpy(arp+22,gwmac,6); memcpy(arp+28,gateway,4); memcpy(arp+32,mac,6); memcpy(arp+38,ip,4);
    unsigned before=c.sent; aic_network_rx(s,arp,sizeof(arp)); assert(c.sent>=before+2);
    const uint8_t *query=c.frame[c.sent-1]+42; size_t qlen=c.length[c.sent-1]-42;
    uint8_t dns[512]={0}; assert(qlen<480); memcpy(dns,query,qlen); dns[2]=0x81; dns[3]=0x80; dns[7]=1;
    const uint8_t answer[]={0xc0,12,0,1,0,1,0,0,0,60,0,4,180,101,50,188};
    memcpy(dns+qlen,answer,sizeof(answer)); size_t dlen=qlen+sizeof(answer);
    uint8_t address[4]; assert(!aic_dns_test_answer(dns,dlen,address));
    assert(address[0]==180 && address[3]==188);
    for(size_t i=0;i<dlen;i++) assert(aic_dns_test_answer(dns,i,address)<0);
    dns[12]^=1; assert(aic_dns_test_answer(dns,dlen,address)<0); dns[12]^=1;
    n=ipv4_udp(frame,53,49180,dns,dlen); aic_network_rx(s,frame,n);
    assert(aic_network_ready(s) && !memcmp(aic_network_test_address(s),address,4));
    uint8_t outgoing[20]={0x45}; put16(outgoing+2,sizeof(outgoing)); memcpy(outgoing+12,ip,4);
    memcpy(outgoing+16,"\x01\x01\x01\x01",4); before=c.sent;
    assert(!aic_network_tx_ip(s,outgoing,sizeof(outgoing)) && c.sent==before+1 && !memcmp(c.frame[before],gwmac,6));
    aic_network_destroy(s);
    puts("IPv4 tests passed: DHCP transaction validation, ARP resolution, compressed DNS parsing, real A record, gateway routing.");
    return 0;
}
