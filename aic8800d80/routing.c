/* SPDX-License-Identifier: GPL-2.0-only */
#include "routing.h"
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <net/route.h>
#include <net/if.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

int aic_routes_parse(const uint8_t *p, size_t n, struct aic_route_entry *out, size_t cap, size_t *count)
{
    *count=0;
    while(n) {
        struct rt_msghdr2 h;
        if(n<sizeof(h)) return -1;
        memcpy(&h,p,sizeof(h));
        if(h.rtm_msglen<sizeof(h) || h.rtm_msglen>n || h.rtm_version!=RTM_VERSION) return -1;
        struct aic_route_entry e={.index=h.rtm_index,.scoped=!!(h.rtm_flags&RTF_IFSCOPE)};
        bool ipv4=false; size_t offset=sizeof(h);
        for(unsigned i=0;i<RTAX_MAX;i++) if(h.rtm_addrs&(1U<<i)) {
            if(h.rtm_msglen-offset<2) return -1;
            const uint8_t *sa=p+offset;
            size_t len=sa[0], aligned=len?((len+3)&~(size_t)3):4;
            if(aligned>h.rtm_msglen-offset) return -1;
            if(i==RTAX_DST && len>=8 && sa[1]==AF_INET) {
                uint32_t address; memcpy(&address,sa+4,4); e.network=ntohl(address); ipv4=true;
            }
            if(i==RTAX_NETMASK && len>4) {
                uint8_t mask[4]={0}; size_t bytes=len-4<4?len-4:4;
                memcpy(mask,sa+4,bytes); uint32_t word; memcpy(&word,mask,4); e.mask=ntohl(word);
            }
            offset+=aligned;
        }
        if(h.rtm_flags&RTF_HOST) e.mask=UINT32_MAX;
        if(ipv4 && (h.rtm_flags&RTF_UP)) {
            if(*count==cap) { errno=EOVERFLOW; return -1; }
            out[(*count)++]=e;
        }
        n-=h.rtm_msglen; p+=h.rtm_msglen;
    }
    return 0;
}
static struct aic_route_entry *snapshot(size_t *count)
{
    int mib[]={CTL_NET,PF_ROUTE,0,AF_INET,NET_RT_DUMP2,0}; size_t size=0;
    if(sysctl(mib,6,NULL,&size,NULL,0)) return NULL;
    size+=size/4+4096;
    uint8_t *data=malloc(size);
    struct aic_route_entry *entries=calloc(8192,sizeof(*entries));
    if(!data || !entries) { free(data); free(entries); return NULL; }
    if(sysctl(mib,6,data,&size,NULL,0) || aic_routes_parse(data,size,entries,8192,count)) {
        free(data); free(entries); return NULL;
    }
    free(data); return entries;
}
bool aic_route_owned(const struct aic_route_entry *e, uint32_t network, uint32_t mask, bool scoped, unsigned index)
{ return e->network==network && e->mask==mask && e->scoped==scoped && e->index==index; }
int aic_route_owner(uint32_t network, uint32_t mask, bool scoped, unsigned scope, unsigned *owner)
{
    size_t n; struct aic_route_entry *entries=snapshot(&n); if(!entries) return -1;
    int found=0; *owner=0;
    for(size_t i=0;i<n;i++) if(entries[i].network==network && entries[i].mask==mask &&
        entries[i].scoped==scoped && (!scoped || entries[i].index==scope)) {
        *owner=entries[i].index; found=1; break;
    }
    free(entries); return found;
}
int aic_route_other_tunnel(unsigned ours, char *name, size_t cap)
{
    size_t n; struct aic_route_entry *entries=snapshot(&n); if(!entries) return -1;
    int found=0;
    for(size_t i=0;i<n;i++) if(entries[i].index!=ours && !entries[i].scoped &&
        entries[i].mask<=0xff000000U) {
        char iface[IFNAMSIZ];
        if(if_indextoname(entries[i].index,iface) && !strncmp(iface,"utun",4)) {
            strlcpy(name,iface,cap); found=1; break;
        }
    }
    free(entries); return found;
}
