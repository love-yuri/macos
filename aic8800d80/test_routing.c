/* SPDX-License-Identifier: GPL-2.0-only */
#include "routing.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <net/route.h>
#include <netinet/in.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static size_t route(uint8_t *p, uint32_t network, uint32_t mask, unsigned index, bool scoped, bool compact)
{
    struct rt_msghdr2 h={.rtm_version=RTM_VERSION,.rtm_flags=RTF_UP|RTF_STATIC|(scoped?RTF_IFSCOPE:0),
        .rtm_index=index,.rtm_addrs=RTA_DST|RTA_NETMASK};
    struct sockaddr_in dst={.sin_len=sizeof(dst),.sin_family=AF_INET,.sin_addr.s_addr=htonl(network)};
    struct sockaddr_in netmask={.sin_len=compact?8:sizeof(netmask),.sin_addr.s_addr=htonl(mask)};
    h.rtm_msglen=sizeof(h)+sizeof(dst)+netmask.sin_len;
    memcpy(p,&h,sizeof(h)); memcpy(p+sizeof(h),&dst,sizeof(dst));
    memcpy(p+sizeof(h)+sizeof(dst),&netmask,netmask.sin_len);
    return h.rtm_msglen;
}
int main(void)
{
    uint8_t dump[2048]={0}; size_t n=0;
    n+=route(dump+n,0,0,5,false,false);
    n+=route(dump+n,0x80000000,0x80000000,10,false,true); /* existing VPN */
    n+=route(dump+n,0,0,11,true,true); /* our scoped route */
    struct aic_route_entry entries[8]; size_t count;
    assert(!aic_routes_parse(dump,n,entries,8,&count) && count==3);
    assert(aic_route_owned(&entries[0],0,0,false,5));
    assert(aic_route_owned(&entries[1],0x80000000,0x80000000,false,10));
    assert(!aic_route_owned(&entries[1],0x80000000,0x80000000,false,11));
    assert(!aic_route_owned(&entries[0],0,0,true,5));
    assert(aic_route_owned(&entries[2],0,0,true,11));
    assert(aic_routes_parse(dump,n-1,entries,8,&count)<0);
    assert(aic_routes_parse(dump,n,entries,1,&count)<0);
    dump[sizeof(struct rt_msghdr2)]=255;
    assert(aic_routes_parse(dump,n,entries,8,&count)<0);
    puts("Routing tests passed: compact masks, exact prefix/scope/owner, foreign VPN preservation, malformed dumps.");
    return 0;
}
