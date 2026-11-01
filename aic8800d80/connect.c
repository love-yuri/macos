/* SPDX-License-Identifier: GPL-2.0-only */
#include "connect.h"
#include "auth.h"
#include "station.h"
#include "datapath.h"
#include "netstack.h"
#include "interface.h"
#include "credentials.h"
#include "lmac_abi.h"
#include "protocol.h"
struct os_reltime;
#include "eloop.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum check_stage { CHECK_NONE, CHECK_LINK, CHECK_SERVICE, CHECK_SYSTEM };
struct connection {
    const struct aic_connection_io *io;
    struct aic_station station;
    struct aic_bss bss;
    struct aic_auth *auth;
    struct aic_network *network;
    struct aic_interface *interface;
    struct aic_lease lease;
    struct aic_rx_stream rx_stream;
    uint8_t ap;
    uint32_t token;
    uint64_t started;
    uint64_t service_started;
    unsigned duration;
    unsigned rx, tx, drops;
    bool associated, authorized, failed, route, routed, probe, success, link_ready;
    enum check_stage checking;
};
static volatile sig_atomic_t stop_requested;
static void request_stop(int sig) { (void)sig; stop_requested=1; }
static int install_stop_handlers(void)
{
    struct sigaction action={0}, hangup;
    action.sa_handler=request_stop;
    sigemptyset(&action.sa_mask);
    if(sigaction(SIGHUP,NULL,&hangup)) return -1;
    if(sigaction(SIGINT,&action,NULL) || sigaction(SIGTERM,&action,NULL) ||
       sigaction(SIGPIPE,&action,NULL)) return -1;
    /* nohup's inherited SIG_IGN must survive initialization. Foreground
     * connections still clean up when their terminal sends SIGHUP. */
    if(hangup.sa_handler!=SIG_IGN && sigaction(SIGHUP,&action,NULL)) return -1;
    sigset_t signals; sigemptyset(&signals); sigaddset(&signals,SIGINT); sigaddset(&signals,SIGTERM);
    sigaddset(&signals,SIGHUP); sigaddset(&signals,SIGPIPE);
    return sigprocmask(SIG_UNBLOCK,&signals,NULL);
}
static uint64_t millis(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static int cmd(struct connection *c, uint16_t id, const void *p, size_t n, void *out, size_t expect)
{ return aic_command(c->io->context,c->io->exchange,id,p,n,out,expect); }
static void fail(void *ctx) { struct connection *c=ctx; c->failed=true; eloop_terminate(); }
static int ether_send(void *ctx, const uint8_t *p, size_t n)
{
    struct connection *c=ctx;
    uint8_t buffer[2048];
    uint32_t token=0;
    if(n>=14 && p[12]==0x88 && p[13]==0x8e) token=0x80000000|(c->token++%64);
    size_t len=aic_tx_encode(buffer,sizeof(buffer),p,n,c->station.vif,c->ap,token);
    if(!len || c->io->send_data(c->io->context,buffer,len)) { fail(c); return -1; }
    c->tx++;
    return 0;
}
static int eapol_send(void *ctx, const uint8_t *dest, const uint8_t *p, size_t n)
{
    struct connection *c=ctx;
    if(n>1500) return -1;
    uint8_t eth[1514]; memcpy(eth,dest,6); memcpy(eth+6,c->station.mac,6);
    eth[12]=0x88; eth[13]=0x8e; memcpy(eth+14,p,n);
    printf("EAPOL TX (%zu bytes).\n",n);
    return ether_send(c,eth,n+14);
}
static int install_key(void *ctx, bool pairwise, unsigned index, const uint8_t *p, size_t n)
{
    struct connection *c=ctx;
    struct mm_key_add_req key={.sta_idx=pairwise?c->ap:0xff,
        .key_idx=pairwise?0:index,.inst_nbr=c->station.vif,.pairwise=pairwise,.cipher_suite=MAC_CIPHER_CCMP};
    struct mm_key_add_cfm cfm;
    if(n!=16) return -1;
    key.key.length=n; memcpy(key.key.array,p,n);
    int rc=cmd(c,MM_KEY_ADD_REQ,&key,sizeof(key),&cfm,sizeof(cfm));
    aic_clear(&key,sizeof(key));
    if(rc || cfm.status) return -1;
    printf("Installed %s CCMP key (hardware slot %u).\n",pairwise?"pairwise":"group",cfm.hw_key_idx);
    return 0;
}
static void utun_rx(int fd, void *ctx, void *unused)
{
    (void)fd; (void)unused;
    struct connection *c=ctx;
    for(unsigned i=0;i<32;i++) {
        uint8_t p[1500]; int n=aic_interface_read(c->interface,p,sizeof(p));
        if(n<0) { fail(c); return; }
        if(!n) return;
        if(!c->network || aic_network_tx_ip(c->network,p,n)) c->drops++;
    }
}
static int lease_received(void *ctx, const struct aic_lease *lease)
{
    struct connection *c=ctx;
    c->lease=*lease;
    if(c->probe) return 0;
    if(aic_interface_configure(c->interface,lease)) return -1;
    return 0;
}
static void ip_received(void *ctx, const uint8_t *p, size_t n)
{
    struct connection *c=ctx;
    if(c->interface && aic_interface_write(c->interface,p,n)) fail(c);
}
static void auth_completed(void *ctx)
{
    struct connection *c=ctx;
    if(c->authorized) return; /* group-key rekey keeps the running DHCP state */
    struct me_set_control_port_req port={.sta_idx=c->ap,.control_port_open=true};
    if(cmd(c,ME_SET_CONTROL_PORT_REQ,&port,sizeof(port),NULL,0)) { fail(c); return; }
    c->authorized=true;
    puts("WPA2 authentication completed; firmware control port is open.");
    const struct aic_network_ops ops={.ctx=c,.send=ether_send,.lease=lease_received,.ip=ip_received,.failure=fail};
    c->network=aic_network_create(&ops,c->station.mac);
    if(!c->network) { fail(c); return; }
    aic_network_tick(c->network,millis());
}
static void ether_received(void *ctx, const uint8_t *p, size_t n, int encrypted)
{
    struct connection *c=ctx;
    c->rx++;
    if(n>=18 && p[12]==0x88 && p[13]==0x8e) {
        if(!c->associated || memcmp(p+6,c->bss.bssid,6) || memcmp(p,c->station.mac,6)) return;
        printf("EAPOL RX (%zu bytes).\n",n-14);
        aic_auth_rx(c->auth,p+6,p+14,n-14,encrypted);
    } else if(c->authorized && encrypted && c->network) aic_network_rx(c->network,p,n);
}
static int message(struct connection *c, const uint8_t *p, size_t n)
{
    for(size_t i=0;i<n;) {
        if(n-i<4) return -1;
        const uint8_t *f=p+i;
        size_t len=aic_get16(f), total=4+((len+3)&~(size_t)3);
        if(!len || total>n-i) return -1;
        if((f[2]&0x7f)==0x11) {
            if(len<12 || aic_get16(f+10)>len-12) return -1;
            uint16_t id=aic_get16(f+4); size_t size=aic_get16(f+10);
            if(id==SM_CONNECT_IND) {
                if(size<offsetof(struct sm_connect_ind,assoc_ie_buf)) return -1;
                struct sm_connect_ind ind={0}; memcpy(&ind,f+16,size<sizeof(ind)?size:sizeof(ind));
                printf("Association result: status=%u VIF=%u AP=%u QoS=%u.\n",ind.status_code,ind.vif_idx,ind.ap_idx,ind.qos);
                if(ind.status_code || ind.vif_idx!=c->station.vif || memcmp(&ind.bssid,c->bss.bssid,6)) return -1;
                c->ap=ind.ap_idx; c->associated=true; aic_auth_associated(c->auth);
            } else if(id==SM_DISCONNECT_IND) {
                if(size>=3 && f[18]==c->station.vif) {
                    fprintf(stderr,"Disconnected by firmware/AP (reason %u).\n",aic_get16(f+16)); return -1;
                }
            } else if(id==SM_EXTERNAL_AUTH_REQUIRED_IND) {
                fprintf(stderr,"AP requested unsupported external authentication.\n"); return -1;
            }
        }
        i+=total;
    }
    return 0;
}
static void poll_usb(void *ctx, void *unused)
{
    (void)unused;
    struct connection *c=ctx;
    if(stop_requested) { eloop_terminate(); return; }
    uint8_t p[65536]; size_t n=0;
    /* Command events must be handled before data so association indices exist
     * before processing the AP's first EAPOL packet. */
    int rc=c->io->receive(c->io->context,p,sizeof(p),&n);
    if(rc<0 || (rc && message(c,p,n))) { fail(c); return; }
    for(unsigned i=0;i<16 && !c->failed;i++) {
        rc=c->io->receive_data(c->io->context,p,sizeof(p),&n);
        if(rc<0) { fail(c); return; }
        if(!rc) break;
        if(aic_rx_feed(&c->rx_stream,p,n,c->station.vif,ether_received,c)) {
            fprintf(stderr,"Invalid USB RX stream (%zu-byte completion).\n",n); c->drops++;
        }
    }
    if(c->network) {
        aic_network_tick(c->network,millis());
        if(aic_network_ready(c->network) && !c->link_ready) {
            c->link_ready=true;
            puts("Verified: WPA2 + DHCP + gateway ARP + DNS over the AIC adapter.");
            if(c->probe) { c->success=true; eloop_terminate(); return; }
            printf("Testing real HTTPS bound to %s before changing system routing...\n",aic_interface_name(c->interface));
            if(aic_interface_check_start(c->interface,aic_network_test_address(c->network))) { fail(c); return; }
            c->checking=CHECK_LINK;
        }
        if(c->checking==CHECK_SERVICE) {
            int ready=aic_interface_service_ready(c->interface);
            if(ready<0 || (!ready && millis()-c->service_started>5000)) {
                fprintf(stderr,"macOS did not activate the AIC primary IPv4/default DNS service; restoring previous networking.\n");
                fail(c); return;
            }
            if(ready) {
                puts("macOS activated the AIC primary IPv4/default DNS service. Checking ordinary HTTPS without --resolve or interface binding...");
                if(aic_interface_check_start(c->interface,NULL)) { fail(c); return; }
                c->checking=CHECK_SYSTEM;
            }
        }
        if(c->checking==CHECK_LINK || c->checking==CHECK_SYSTEM) {
            int checked=aic_interface_check_poll(c->interface);
            if(checked<0) {
                if(c->checking==CHECK_SYSTEM)
                    fprintf(stderr,"System DNS/HTTPS verification failed; removing the temporary AIC network service and routes.\n");
                else
                    fprintf(stderr,"AIC HTTPS verification failed; system default route and DNS were not changed.\n");
                fail(c); return;
            }
            if(checked>0) {
                enum check_stage completed=c->checking;
                c->checking=CHECK_NONE;
                if(completed==CHECK_SYSTEM) {
                    c->success=true;
                    puts("AIC primary network service, system DNS, and ordinary HTTPS verified.");
                } else if(c->route) {
                    printf("HTTPS through %s verified; enabling the AIC network service...\n",aic_interface_name(c->interface));
                    int routed=aic_interface_route(c->interface,&c->lease);
                    if(routed<0) { fail(c); return; }
                    c->routed=routed==0;
                    if(c->routed) {
                        c->service_started=millis();
                        c->checking=CHECK_SERVICE;
                    } else c->success=true;
                } else {
                    c->success=true;
                    printf("Scoped HTTPS through %s verified.\n",aic_interface_name(c->interface));
                }
            }
        }
    }
    if(!c->success && millis()-c->started>75000) {
        fprintf(stderr,"Connection verification timed out.\n"); fail(c); return;
    }
    if(c->duration && millis()-c->started>=(uint64_t)c->duration*1000) {
        puts("Requested connection duration reached; cleaning up."); eloop_terminate(); return;
    }
    if(!c->failed) eloop_register_timeout(0,1000,poll_usb,c,NULL);
}
int aic_connect(const struct aic_connection_io *io, const char *ssid, bool route, bool probe, unsigned duration,
                const char *credential_file)
{
    struct connection c={.io=io,.route=route,.probe=probe,.duration=duration};
    int result=-1;
    if(!probe && geteuid()!=0) {
        fprintf(stderr,"macOS network interface creation requires sudo. Use --probe yes for association/DHCP diagnostics only.\n");
        return -1;
    }
    if(eloop_init()) return -1;
    /* Terminal window close sends SIGHUP, including during initialization.
     * Keep cleanup in normal control flow so station/USB state is released. */
    stop_requested=0;
    if(install_stop_handlers()) goto done;
    if(!probe && !(c.interface=aic_interface_open())) goto done;
    if(aic_station_start(io->context,io->exchange,&c.station) ||
        aic_find_bss(io->context,io->exchange,io->receive,&c.station,ssid,&c.bss) ||
        aic_auth_validate(c.bss.rsn,c.bss.rsn_len)) goto done;
    if(stop_requested) goto done;
    printf("AP RSN capabilities validated: WPA2-PSK / AES-CCMP.\n");
    char password[256]={0};
    int credential=credential_file ? aic_read_password_file(credential_file,ssid,password,sizeof(password)) :
        aic_read_password(ssid,password,sizeof(password));
    if(credential || stop_requested) {
        aic_clear(password,sizeof(password)); goto done;
    }
    const struct aic_auth_ops ops={.ctx=&c,.key=install_key,.send=eapol_send,.complete=auth_completed,.failed=fail};
    c.auth=aic_auth_create(&ops,c.station.mac,c.bss.bssid,ssid,password,c.bss.rsn,c.bss.rsn_len);
    aic_clear(password,sizeof(password));
    if(!c.auth) goto done;
    /* USB IN queues survive reopening the user client. Remove bytes/events
     * from the previous station before requesting a new association; never
     * carry an old partial aggregate into the new AP's EAPOL exchange. */
    uint8_t stale[65536]; size_t stale_size;
    for(unsigned i=0;i<128;i++) {
        int read=io->receive_data(io->context,stale,sizeof(stale),&stale_size);
        if(read<0) goto done;
        if(!read) break;
        if(i==127) { fprintf(stderr,"Old USB data queue did not drain.\n"); goto done; }
    }
    for(unsigned i=0;i<256;i++) {
        int read=io->receive(io->context,stale,sizeof(stale),&stale_size);
        if(read<0) goto done;
        if(!read) break;
        if(i==255) { fprintf(stderr,"Old firmware event queue did not drain.\n"); goto done; }
    }
    c.rx_stream.used=0;
    struct sm_connect_req req={.vif_idx=c.station.vif,.listen_interval=1,
        .flags=CONTROL_PORT_HOST|CONTROL_PORT_NO_ENC|WPA_WPA2_IN_USE|DISABLE_HT,
        .ctrl_port_ethertype=0x8e88};
    req.ssid.length=strlen(ssid); memcpy(req.ssid.array,ssid,req.ssid.length);
    memcpy(&req.bssid,c.bss.bssid,6);
    req.chan=(struct mac_chan_def){.freq=c.bss.frequency,.band=1,.tx_power=18};
    size_t ie_len=0; const uint8_t *ie=aic_auth_ie(c.auth,&ie_len);
    if(ie_len>sizeof(req.ie_buf)) goto done;
    memcpy(req.ie_buf,ie,ie_len); req.ie_len=ie_len;
    struct sm_connect_cfm cfm;
    if(cmd(&c,SM_CONNECT_REQ,&req,sizeof(req),&cfm,sizeof(cfm)) || cfm.status) goto done;
    puts("Association request accepted by firmware; waiting for AP and WPA2 authentication...");
    if(c.interface && eloop_register_read_sock(aic_interface_fd(c.interface),utun_rx,&c,NULL)) goto done;
    c.started=millis();
    eloop_register_timeout(0,0,poll_usb,&c,NULL);
    eloop_run();
    result=c.success && !c.failed?0:-1;
done:
    if(c.interface) eloop_unregister_read_sock(aic_interface_fd(c.interface));
    aic_interface_close(c.interface);
    aic_auth_destroy(c.auth);
    aic_network_destroy(c.network);
    aic_station_stop(io->context,io->exchange,&c.station);
    eloop_destroy();
    printf("Connection stopped: TX=%u RX=%u dropped=%u.\n",c.tx,c.rx,c.drops);
    return result;
}
