/* SPDX-License-Identifier: GPL-2.0-only */
#include "interface.h"
#include "routing.h"
#include "service.h"
#include <SystemConfiguration/SystemConfiguration.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/sys_domain.h>
#include <sys/wait.h>
#include <net/if_utun.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

struct aic_interface {
    int fd; char name[IFNAMSIZ]; unsigned index;
    bool scoped_default;
    pid_t check;
    SCDynamicStoreRef service;
    uint8_t nameserver[4];
};
static int publish_service(struct aic_interface *s, const struct aic_lease *lease)
{
    char identifier[64]; snprintf(identifier,sizeof(identifier),"AIC8800-%d",getpid());
    CFDictionaryRef values=aic_service_values_create(identifier,s->name,lease);
    if(!values) return -1;
    bool ok=SCDynamicStoreSetMultiple(s->service,values,NULL,NULL);
    CFRelease(values);
    if(!ok) fprintf(stderr,"Publish AIC IPv4/DNS service: %s\n",SCErrorString(SCError()));
    else memcpy(s->nameserver,lease->dns,sizeof(s->nameserver));
    return ok?0:-1;
}
static int run(char *const argv[])
{
    pid_t pid=fork();
    if(pid<0) return -1;
    if(!pid) { execv(argv[0],argv); _exit(127); }
    int status;
    while(waitpid(pid,&status,0)<0) if(errno!=EINTR) return -1;
    return WIFEXITED(status) && WEXITSTATUS(status)==0 ? 0 : -1;
}
struct aic_interface *aic_interface_open(void)
{
    struct aic_interface *s=calloc(1,sizeof(*s)); if(!s) return NULL;
    s->fd=socket(PF_SYSTEM,SOCK_DGRAM,SYSPROTO_CONTROL);
    if(s->fd<0) goto fail;
    struct ctl_info info={0}; strlcpy(info.ctl_name,UTUN_CONTROL_NAME,sizeof(info.ctl_name));
    if(ioctl(s->fd,CTLIOCGINFO,&info)) goto fail;
    struct sockaddr_ctl addr={.sc_len=sizeof(addr),.sc_family=AF_SYSTEM,
        .ss_sysaddr=AF_SYS_CONTROL,.sc_id=info.ctl_id};
    if(connect(s->fd,(struct sockaddr *)&addr,sizeof(addr))) goto fail;
    socklen_t n=sizeof(s->name);
    if(getsockopt(s->fd,SYSPROTO_CONTROL,UTUN_OPT_IFNAME,s->name,&n)) goto fail;
    s->index=if_nametoindex(s->name);
    if(!s->index) goto fail;
    fcntl(s->fd,F_SETFD,FD_CLOEXEC);
    fcntl(s->fd,F_SETFL,O_NONBLOCK);
    printf("Created native macOS interface %s.\n",s->name);
    return s;
fail:
    perror("Create utun (requires sudo)");
    if(s->fd>=0) close(s->fd);
    free(s); return NULL;
}
int aic_interface_configure(struct aic_interface *s, const struct aic_lease *lease)
{
    char ip[16],gw[16],mask[16];
    inet_ntop(AF_INET,lease->ip,ip,sizeof(ip)); inet_ntop(AF_INET,lease->gateway,gw,sizeof(gw));
    inet_ntop(AF_INET,lease->mask,mask,sizeof(mask));
    char *args[]={"/sbin/ifconfig",s->name,"inet",ip,gw,"netmask",mask,"mtu","1400","up",NULL};
    if(run(args)) return -1;
    if(!s->scoped_default) {
        unsigned owner=0;
        if(aic_route_owner(0,0,true,s->index,&owner)!=0) {
            fprintf(stderr,"Scoped route already exists or could not be inspected.\n"); return -1;
        }
        char *add[]={"/sbin/route","-n","add","-ifscope",s->name,"default","-interface",s->name,NULL};
        int result=run(add);
        int present=aic_route_owner(0,0,true,s->index,&owner);
        s->scoped_default=present==1 && owner==s->index;
        if(result || !s->scoped_default) return -1;
    }
    if(s->service) return publish_service(s,lease); /* DHCP renewal */
    printf("Scoped IPv4 route ready on %s; system default route and DNS unchanged.\n",s->name);
    return 0;
}
static void unroute(struct aic_interface *s)
{
    /* Releasing the session removes BOTH entities. configd re-elects the
     * previous service, including its default route, NWI state, and DNS. */
    if(s->service) { CFRelease(s->service); s->service=NULL; }
}
int aic_interface_route(struct aic_interface *s, const struct aic_lease *lease)
{
    if(s->service) return 0;
    /* configd installs the underlying default from the IPv4 service below.
     * Split defaults belong to VPN/TUN applications: installing our own /1
     * routes prevents Clash from taking over traffic on top of this link. */
    const void *keys[]={kSCDynamicStoreUseSessionKeys}; const void *vals[]={kCFBooleanTrue};
    CFDictionaryRef options=CFDictionaryCreate(NULL,keys,vals,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    s->service=SCDynamicStoreCreateWithOptions(NULL,CFSTR("AIC8800 native networking"),options,NULL,NULL);
    CFRelease(options);
    if(!s->service || publish_service(s,lease)) { unroute(s); return -1; }
    printf("Temporary IPv4 network service and default DNS published for %s. Ctrl-C restores previous networking.\n",s->name);
    return 0;
}
const char *aic_interface_name(struct aic_interface *s) { return s->name; }
int aic_interface_service_ready(struct aic_interface *s)
{
    if(!s->service) return -1;
    CFDictionaryRef ipv4=SCDynamicStoreCopyValue(s->service,CFSTR("State:/Network/Global/IPv4"));
    CFDictionaryRef dns=SCDynamicStoreCopyValue(s->service,CFSTR("State:/Network/Global/DNS"));
    CFStringRef identifier=CFStringCreateWithFormat(NULL,NULL,CFSTR("AIC8800-%d"),getpid());
    bool ready=false;
    if(ipv4 && dns && identifier && CFGetTypeID(ipv4)==CFDictionaryGetTypeID() &&
       CFGetTypeID(dns)==CFDictionaryGetTypeID()) {
        CFTypeRef primary=CFDictionaryGetValue(ipv4,kSCDynamicStorePropNetPrimaryService);
        CFArrayRef servers=CFDictionaryGetValue(dns,kSCPropNetDNSServerAddresses);
        if(primary && CFEqual(primary,identifier) && servers && CFGetTypeID(servers)==CFArrayGetTypeID()) {
            char address[16]; inet_ntop(AF_INET,s->nameserver,address,sizeof(address));
            CFStringRef server=CFStringCreateWithCString(NULL,address,kCFStringEncodingUTF8);
            if(server) {
                ready=CFArrayContainsValue(servers,CFRangeMake(0,CFArrayGetCount(servers)),server);
                CFRelease(server);
            }
        }
    }
    if(ipv4) CFRelease(ipv4);
    if(dns) CFRelease(dns);
    if(identifier) CFRelease(identifier);
    return ready?1:0;
}
int aic_interface_check_start(struct aic_interface *s, const uint8_t *ip)
{
    if(s->check) return -1;
    char address[16], resolve[64];
    if(ip) {
        inet_ntop(AF_INET,ip,address,sizeof(address));
        snprintf(resolve,sizeof(resolve),"www.baidu.com:443:%s",address);
    }
    s->check=fork();
    if(s->check<0) { s->check=0; return -1; }
    if(!s->check) {
        char *args[40]; unsigned n=0;
        args[n++]="/usr/bin/curl"; args[n++]="--disable";
        if(ip) {
            args[n++]="--interface"; args[n++]=s->name;
            args[n++]="--resolve"; args[n++]=resolve;
        } else {
            /* Give configd/mDNSResponder a moment to consume the service.
             * This check must use the host resolver and ordinary routing. */
            args[n++]="--retry"; args[n++]="2";
            args[n++]="--retry-all-errors"; args[n++]="--retry-delay"; args[n++]="1";
            args[n++]="--retry-max-time"; args[n++]="5";
        }
        args[n++]="--noproxy"; args[n++]="*"; args[n++]="--ipv4";
        args[n++]="--connect-timeout"; args[n++]="7"; args[n++]="--max-time"; args[n++]="20";
        args[n++]="--silent"; args[n++]="--show-error"; args[n++]="--fail";
        args[n++]="--output"; args[n++]="/dev/null"; args[n++]="--write-out";
        args[n++]=ip?"AIC link HTTPS check: HTTP %{http_code}, local %{local_ip}, remote %{remote_ip}\\n":
            "AIC system DNS/HTTPS check: HTTP %{http_code}, local %{local_ip}, remote %{remote_ip}\\n";
        args[n++]="https://www.baidu.com/"; args[n]=NULL;
        execv(args[0],args);
        _exit(127);
    }
    return 0;
}
int aic_interface_check_poll(struct aic_interface *s)
{
    if(!s->check) return -1;
    int status; pid_t pid=waitpid(s->check,&status,WNOHANG);
    if(pid==0 || (pid<0 && errno==EINTR)) return 0;
    s->check=0;
    return pid>0 && WIFEXITED(status) && WEXITSTATUS(status)==0 ? 1 : -1;
}
int aic_interface_fd(struct aic_interface *s) { return s->fd; }
int aic_interface_write(struct aic_interface *s, const uint8_t *p, size_t n)
{
    if(n>1500) return -1;
    uint8_t buf[1504]; uint32_t af=htonl(AF_INET); memcpy(buf,&af,4); memcpy(buf+4,p,n);
    ssize_t written=write(s->fd,buf,n+4);
    if(written<0 && (errno==EAGAIN || errno==ENOBUFS)) return 0;
    return written==(ssize_t)n+4 ? 0 : -1;
}
int aic_interface_read(struct aic_interface *s, uint8_t *p, size_t n)
{
    uint8_t buf[1604]; ssize_t got=read(s->fd,buf,sizeof(buf));
    if(got<0 && (errno==EAGAIN || errno==EINTR)) return 0;
    if(got<4) return -1;
    uint32_t af; memcpy(&af,buf,4);
    if(ntohl(af)!=AF_INET) return 0;
    if((size_t)got-4>n) return -1;
    memcpy(p,buf+4,got-4); return (int)got-4;
}
void aic_interface_close(struct aic_interface *s)
{
    if(!s) return;
    if(s->check>0) {
        kill(s->check,SIGTERM);
        while(waitpid(s->check,NULL,0)<0 && errno==EINTR) {}
    }
    unroute(s);
    /* A unique utun's scoped route is removed by the kernel on close. */
    close(s->fd); /* kernel also removes routes referencing the vanished utun */
    free(s);
}
