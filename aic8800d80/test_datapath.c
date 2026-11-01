/* SPDX-License-Identifier: GPL-2.0-only */
#include "datapath.h"
#include "protocol.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
struct capture { unsigned count; uint8_t eth[1600]; size_t length; int encrypted; };
static void receive(void *ctx, const uint8_t *p, size_t n, int enc)
{ struct capture *c=ctx; assert(n<=sizeof(c->eth)); c->count++; memcpy(c->eth,p,n); c->length=n; c->encrypted=enc; }
static size_t packet(uint8_t *p, bool encrypted)
{
    memset(p,0,192);
    size_t header=24+(encrypted?8:0), len=header+8+20+8;
    aic_put16(p,len); aic_put32(p+48,1<<6); aic_put32(p+36,encrypted?3<<2:0);
    uint8_t *f=p+60; f[0]=8; f[1]=2|(encrypted?0x40:0);
    memcpy(f+4,"\x02\x00\x00\x00\x00\x01",6);
    memcpy(f+16,"\x02\x00\x00\x00\x00\x02",6);
    memcpy(f+header,"\xaa\xaa\x03\x00\x00\x00\x08\x00",8);
    f[header+8]=0x45; f[header+11]=20;
    return len+60;
}
int main(void)
{
    uint8_t wire[4096], eth[1514]={0};
    memcpy(eth,"\x02\x00\x00\x00\x00\x02\x02\x00\x00\x00\x00\x01\x08\x00",14);
    for(unsigned i=14;i<sizeof(eth);i++) eth[i]=(uint8_t)i;
    assert(!aic_tx_encode(wire,sizeof(wire),eth,13,0,0,0));
    assert(!aic_tx_encode(wire,32,eth,100,0,0,0));
    size_t len=aic_tx_encode(wire,sizeof(wire),eth,494,2,7,0x80000001);
    assert(len==513 && aic_get16(wire)==512 && wire[512]==0);
    assert(aic_get32(wire+8)==0x80000001 && wire[28]==2 && wire[29]==7);
    assert(!memcmp(wire+12,eth,14) && !memcmp(wire+32,eth+14,480));
    uint8_t input[512]; struct capture c={0};
    len=packet(input,true);
    assert(!aic_rx_decode(input,len,0,receive,&c) && c.count==1 && c.encrypted && c.length==34);
    assert(c.eth[0]==2 && c.eth[5]==1 && c.eth[11]==2 && c.eth[14]==0x45);
    for(size_t i=1;i<len;i++) { c.count=0; assert(aic_rx_decode(input,i,0,receive,&c)<0 && !c.count); }
    c.count=0; assert(!aic_rx_decode(input,len,1,receive,&c) && !c.count);
    aic_put32(input+36,0); assert(!aic_rx_decode(input,len,0,receive,&c) && !c.count);
    len=packet(input,false); assert(!aic_rx_decode(input,len,0,receive,&c) && c.count==1 && !c.encrypted);
    size_t second=packet(input+len,true); c.count=0;
    assert(!aic_rx_decode(input,len+second,0,receive,&c) && c.count==2);
    struct aic_rx_stream stream={0}; size_t combined=len+second;
    for(size_t split=1;split<combined;split++) {
        stream.used=0; c.count=0;
        assert(!aic_rx_feed(&stream,input,split,0,receive,&c));
        assert(!aic_rx_feed(&stream,input+split,combined-split,0,receive,&c));
        assert(c.count==2 && !stream.used);
    }
    stream.used=0; c.count=0;
    for(size_t i=0;i<combined;i++) assert(!aic_rx_feed(&stream,input+i,1,0,receive,&c));
    assert(c.count==2 && !stream.used);
    puts("Data path tests passed: Ethernet descriptors, USB boundary, aggregation, CCMP, truncation, VIF isolation.");
    return 0;
}
