/* SPDX-License-Identifier: GPL-2.0-only */
/* IPv4/Ethernet adaptation for macOS utun. TCP/IP stays in macOS. */
#include "netstack.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct aic_network {
    struct aic_network_ops ops;
    uint8_t mac[6];
    uint32_t xid;
    struct aic_lease lease;
    unsigned state, attempts;
    uint64_t now, last_dhcp, acquired, last_arp;
    bool gateway_ok, dns_ok;
    uint8_t test_address[4];
    struct { uint8_t ip[4], mac[6]; bool valid; } arp[32];
    unsigned arp_next;
    struct { uint8_t target[4], data[1500]; size_t length; } pending[16];
};
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0]<<8)|p[1]); }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static void put16(uint8_t *p, unsigned n) { p[0]=n>>8; p[1]=n; }
static void put32(uint8_t *p, uint32_t n) { p[0]=n>>24; p[1]=n>>16; p[2]=n>>8; p[3]=n; }
static const uint8_t test_question[]={3,'w','w','w',5,'b','a','i','d','u',3,'c','o','m',0,0,1,0,1};
/* Skip wire-format names without following compression pointers. Pointer
 * targets are not dereferenced; no unbounded recursion or pointer loops. */
static int dns_skip_name(const uint8_t *p, size_t n, size_t *offset)
{
    for(unsigned labels=0;labels<128 && *offset<n;labels++) {
        unsigned len=p[(*offset)++];
        if(!len) return 0;
        if((len&0xc0)==0xc0) {
            if(*offset>=n || (((len&63)<<8)|p[*offset])>=n) return -1;
            (*offset)++; return 0;
        }
        if(len>63 || len>n-*offset) return -1;
        *offset+=len;
    }
    return -1;
}
int aic_dns_test_answer(const uint8_t *p, size_t n, uint8_t *ip)
{
    if(n<12+sizeof(test_question) || !(p[2]&0x80) || (p[2]&0x7a) || (p[3]&15) || be16(p+4)!=1 ||
        memcmp(p+12,test_question,sizeof(test_question))) return -1;
    size_t offset=12+sizeof(test_question);
    unsigned answers=be16(p+6);
    for(unsigned i=0;i<answers;i++) {
        if(dns_skip_name(p,n,&offset) || n-offset<10) return -1;
        unsigned type=be16(p+offset), cls=be16(p+offset+2), length=be16(p+offset+8);
        offset+=10;
        if(length>n-offset) return -1;
        if(type==1 && cls==1 && length==4 && p[offset]>0 && p[offset]<224 && p[offset]!=127) {
            memcpy(ip,p+offset,4); return 0;
        }
        offset+=length;
    }
    return -1;
}
uint16_t aic_checksum(const uint8_t *p, size_t n)
{
    uint32_t sum=0;
    while(n>1) { sum+=be16(p); p+=2; n-=2; }
    if(n) sum+=(unsigned)*p<<8;
    while(sum>>16) sum=(sum&65535)+(sum>>16);
    return (uint16_t)~sum;
}
static int send_eth(struct aic_network *s, const uint8_t *mac, uint16_t type, const uint8_t *p, size_t n)
{
    if(n>1500) return -1;
    uint8_t frame[1514];
    memcpy(frame,mac,6); memcpy(frame+6,s->mac,6); put16(frame+12,type); memcpy(frame+14,p,n);
    return s->ops.send(s->ops.ctx,frame,n+14);
}
static void arp_request(struct aic_network *s, const uint8_t *ip)
{
    uint8_t p[28]={0}, bc[6]; memset(bc,255,6);
    put16(p,1); put16(p+2,0x800); p[4]=6; p[5]=4; put16(p+6,1);
    memcpy(p+8,s->mac,6); memcpy(p+14,s->lease.ip,4); memcpy(p+24,ip,4);
    send_eth(s,bc,0x806,p,sizeof(p)); s->last_arp=s->now;
}
static void dhcp_send(struct aic_network *s, bool request)
{
    uint8_t pkt[20+8+320]={0}, *udp=pkt+20, *p=udp+8;
    p[0]=1; p[1]=1; p[2]=6; put32(p+4,s->xid); put16(p+10,0x8000);
    memcpy(p+28,s->mac,6); put32(p+236,0x63825363);
    size_t k=240;
    p[k++]=53; p[k++]=1; p[k++]=request?3:1;
    p[k++]=61; p[k++]=7; p[k++]=1; memcpy(p+k,s->mac,6); k+=6;
    if(request) {
        p[k++]=50; p[k++]=4; memcpy(p+k,s->lease.ip,4); k+=4;
        p[k++]=54; p[k++]=4; memcpy(p+k,s->lease.server,4); k+=4;
    }
    p[k++]=55; p[k++]=5; p[k++]=1; p[k++]=3; p[k++]=6; p[k++]=51; p[k++]=54;
    p[k++]=255;
    if(k<300) k=300;
    pkt[0]=0x45; put16(pkt+2,20+8+k); pkt[8]=64; pkt[9]=17;
    memset(pkt+16,255,4); put16(pkt+10,aic_checksum(pkt,20));
    put16(udp,68); put16(udp+2,67); put16(udp+4,8+k);
    uint8_t bc[6]; memset(bc,255,6);
    send_eth(s,bc,0x800,pkt,28+k);
    s->last_dhcp=s->now; s->attempts++;
    puts(request?"DHCP: REQUEST sent.":"DHCP: DISCOVER sent.");
}
static void dhcp_rx(struct aic_network *s, const uint8_t *p, size_t n)
{
    if(n<240 || p[0]!=2 || p[1]!=1 || p[2]!=6 || be32(p+4)!=s->xid ||
        memcmp(p+28,s->mac,6) || be32(p+236)!=0x63825363) return;
    struct aic_lease lease={0}; memcpy(lease.ip,p+16,4);
    unsigned type=0;
    for(size_t i=240;i<n;) {
        uint8_t option=p[i++];
        if(option==255) break;
        if(!option) continue;
        if(i>=n) return;
        size_t len=p[i++]; if(len>n-i) return;
        switch(option) {
        case 53: if(len!=1) return; type=p[i]; break;
        case 1: if(len!=4) return; memcpy(lease.mask,p+i,4); break;
        case 3: if(len<4 || len%4) return; memcpy(lease.gateway,p+i,4); break;
        case 6: if(len<4 || len%4) return; memcpy(lease.dns,p+i,4); break;
        case 54: if(len!=4) return; memcpy(lease.server,p+i,4); break;
        case 51: if(len!=4) return; lease.seconds=be32(p+i); break;
        }
        i+=len;
    }
    if(type==6 && s->state==1 && !memcmp(lease.server,s->lease.server,4)) {
        fprintf(stderr,"DHCP: server rejected the lease.\n"); s->ops.failure(s->ops.ctx); return;
    }
    if(!be32(lease.ip) || !be32(lease.server)) return;
    if(type==2 && s->state==0) {
        s->lease=lease; s->state=1; s->attempts=0; dhcp_send(s,true);
    } else if(type==5 && s->state==1 && !memcmp(lease.server,s->lease.server,4) &&
              !memcmp(lease.ip,s->lease.ip,4)) {
        if(!be32(lease.mask) || !be32(lease.gateway) || !be32(lease.dns) || lease.seconds<60) return;
        s->lease=lease; s->state=2; s->acquired=s->now;
        char ip[16], gw[16], dns_address[16];
        inet_ntop(AF_INET,lease.ip,ip,sizeof(ip)); inet_ntop(AF_INET,lease.gateway,gw,sizeof(gw));
        inet_ntop(AF_INET,lease.dns,dns_address,sizeof(dns_address));
        printf("DHCP ACK: address=%s gateway=%s DNS=%s lease=%u seconds.\n",ip,gw,dns_address,lease.seconds);
        if(s->ops.lease(s->ops.ctx,&s->lease)) { s->ops.failure(s->ops.ctx); return; }
        arp_request(s,lease.gateway);
        /* Verify the router and DNS directly over this adapter, independently
         * of the host's current default route. */
        uint8_t ping[28]={0x45}; put16(ping+2,sizeof(ping)); ping[8]=64; ping[9]=1;
        memcpy(ping+12,lease.ip,4); memcpy(ping+16,lease.gateway,4);
        put16(ping+10,aic_checksum(ping,20)); ping[20]=8; put16(ping+24,0xa1c8);
        put16(ping+22,aic_checksum(ping+20,8)); aic_network_tx_ip(s,ping,sizeof(ping));
        uint8_t dns[28+12+sizeof(test_question)]={0x45}; put16(dns+2,sizeof(dns)); dns[8]=64; dns[9]=17;
        memcpy(dns+12,lease.ip,4); memcpy(dns+16,lease.dns,4); put16(dns+10,aic_checksum(dns,20));
        put16(dns+20,49180); put16(dns+22,53); put16(dns+24,sizeof(dns)-20);
        const uint8_t query[12]={0xa1,0xc8,1,0,0,1};
        memcpy(dns+28,query,sizeof(query)); memcpy(dns+40,test_question,sizeof(test_question));
        aic_network_tx_ip(s,dns,sizeof(dns));
    }
}
struct aic_network *aic_network_create(const struct aic_network_ops *ops, const uint8_t *mac)
{
    struct aic_network *s=calloc(1,sizeof(*s)); if(!s) return NULL;
    s->ops=*ops; memcpy(s->mac,mac,6); s->xid=arc4random();
    return s;
}
void aic_network_destroy(struct aic_network *s) { free(s); }
bool aic_network_ready(struct aic_network *s) { return s && s->gateway_ok && s->dns_ok; }
const uint8_t *aic_network_test_address(struct aic_network *s) { return s->test_address; }
int aic_network_tx_ip(struct aic_network *s, const uint8_t *p, size_t n)
{
    if(s->state!=2 || n<20 || n>1500 || (p[0]>>4)!=4 || be16(p+2)!=n) return -1;
    const uint8_t *target=s->lease.gateway;
    bool local=true;
    for(unsigned i=0;i<4;i++) if((p[16+i]&s->lease.mask[i])!=(s->lease.ip[i]&s->lease.mask[i])) local=false;
    if(local) target=p+16;
    for(unsigned i=0;i<32;i++) if(s->arp[i].valid && !memcmp(s->arp[i].ip,target,4))
        return send_eth(s,s->arp[i].mac,0x800,p,n);
    for(unsigned i=0;i<16;i++) if(!s->pending[i].length) {
        memcpy(s->pending[i].target,target,4); memcpy(s->pending[i].data,p,n); s->pending[i].length=n;
        arp_request(s,target); return 0;
    }
    return -1;
}
void aic_network_rx(struct aic_network *s, const uint8_t *eth, size_t n)
{
    if(n<14 || memcmp(eth+6,s->mac,6)==0) return;
    if(memcmp(eth,s->mac,6) && !(eth[0]&1)) return;
    const uint8_t *p=eth+14; n-=14;
    if(be16(eth+12)==0x806 && s->state==2) {
        if(n<28 || be16(p)!=1 || be16(p+2)!=0x800 || p[4]!=6 || p[5]!=4 ||
            (be16(p+6)!=1 && be16(p+6)!=2) || (p[8]&1) || memcmp(p+8,eth+6,6)) return;
        unsigned k=s->arp_next++%32;
        for(unsigned j=0;j<32;j++) if(s->arp[j].valid && !memcmp(s->arp[j].ip,p+14,4)) { k=j; break; }
        s->arp[k].valid=true; memcpy(s->arp[k].ip,p+14,4); memcpy(s->arp[k].mac,p+8,6);
        if(!s->gateway_ok && !memcmp(p+14,s->lease.gateway,4)) {
            s->gateway_ok=true; puts("Gateway ARP reply received through AIC USB Wi-Fi.");
        }
        if(be16(p+6)==1 && !memcmp(p+24,s->lease.ip,4)) {
            uint8_t reply[28]; memcpy(reply,p,28); put16(reply+6,2);
            memcpy(reply+18,p+8,6); memcpy(reply+24,p+14,4);
            memcpy(reply+8,s->mac,6); memcpy(reply+14,s->lease.ip,4);
            send_eth(s,eth+6,0x806,reply,28);
        }
        for(unsigned j=0;j<16;j++) if(s->pending[j].length && !memcmp(s->pending[j].target,p+14,4)) {
            send_eth(s,p+8,0x800,s->pending[j].data,s->pending[j].length); s->pending[j].length=0;
        }
    } else if(be16(eth+12)==0x800) {
        if(n<20 || (p[0]>>4)!=4) return;
        size_t h=(p[0]&15)*4, len=be16(p+2);
        if(h<20 || h>n || len<h || len>n || aic_checksum(p,h)) return;
        if(p[9]==17 && !(be16(p+6)&0x3fff) && len>=h+8) {
            const uint8_t *udp=p+h; size_t un=be16(udp+4);
            if(un<8 || un>len-h) return;
            if(be16(udp)==67 && be16(udp+2)==68) { dhcp_rx(s,udp+8,un-8); return; }
            if(s->state==2 && !memcmp(p+12,s->lease.dns,4) && !memcmp(p+16,s->lease.ip,4) &&
                be16(udp)==53 && be16(udp+2)==49180 && un>=20 && be16(udp+8)==0xa1c8 &&
                !s->dns_ok && !aic_dns_test_answer(udp+8,un-8,s->test_address)) {
                char address[16]; inet_ntop(AF_INET,s->test_address,address,sizeof(address));
                s->dns_ok=true;
                printf("DNS response through AIC USB Wi-Fi: www.baidu.com = %s.\n",address);
            }
        }
        if(s->state!=2 || memcmp(p+16,s->lease.ip,4)) return;
        if(p[9]==1 && len>=h+8 && p[h]==0 && be16(p+h+4)==0xa1c8 && !memcmp(p+12,s->lease.gateway,4))
            puts("Gateway ICMP echo reply received through AIC USB Wi-Fi.");
        s->ops.ip(s->ops.ctx,p,len);
    }
}
void aic_network_tick(struct aic_network *s, uint64_t now)
{
    s->now=now;
    if(s->state<2 && (!s->attempts || now-s->last_dhcp>=2000)) {
        if(s->attempts>=8) { fprintf(stderr,"DHCP timed out.\n"); s->ops.failure(s->ops.ctx); return; }
        dhcp_send(s,s->state==1);
    }
    if(s->state==2) {
        if(now-s->acquired>=(uint64_t)s->lease.seconds*1000) {
            fprintf(stderr,"DHCP lease expired.\n"); s->ops.failure(s->ops.ctx); return;
        }
        /* Renew by a fresh broadcast REQUEST before expiration. */
        if(now-s->acquired>=(uint64_t)s->lease.seconds*500) {
            s->state=1; s->attempts=0; dhcp_send(s,true);
        }
        if(now-s->last_arp>=1000)
            for(unsigned i=0;i<16;i++) if(s->pending[i].length) { arp_request(s,s->pending[i].target); break; }
    }
}
