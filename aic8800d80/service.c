/* SPDX-License-Identifier: GPL-2.0-only */
#include "service.h"
#include <SystemConfiguration/SystemConfiguration.h>
#include <arpa/inet.h>

static bool set_address(CFMutableDictionaryRef dict, CFStringRef key, const uint8_t *ip, bool array)
{
    char address[INET_ADDRSTRLEN];
    if(!inet_ntop(AF_INET,ip,address,sizeof(address))) return false;
    CFStringRef value=CFStringCreateWithCString(NULL,address,kCFStringEncodingUTF8);
    if(!value) return false;
    if(array) {
        const void *values[]={value};
        CFArrayRef addresses=CFArrayCreate(NULL,values,1,&kCFTypeArrayCallBacks);
        if(!addresses) { CFRelease(value); return false; }
        CFDictionarySetValue(dict,key,addresses);
        CFRelease(addresses);
    } else CFDictionarySetValue(dict,key,value);
    CFRelease(value);
    return true;
}

CFDictionaryRef aic_service_values_create(const char *identifier, const char *name, const struct aic_lease *lease)
{
    if(!identifier || !*identifier || !name || !*name || !lease) return NULL;
    CFStringRef service=CFStringCreateWithCString(NULL,identifier,kCFStringEncodingUTF8);
    CFStringRef iface=CFStringCreateWithCString(NULL,name,kCFStringEncodingUTF8);
    CFMutableDictionaryRef ipv4=CFDictionaryCreateMutable(NULL,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef dns=CFDictionaryCreateMutable(NULL,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef values=CFDictionaryCreateMutable(NULL,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    int override=1, order=1000;
    CFNumberRef primary=CFNumberCreate(NULL,kCFNumberIntType,&override);
    CFNumberRef priority=CFNumberCreate(NULL,kCFNumberIntType,&order);
    CFStringRef ip_key=NULL, dns_key=NULL;
    bool ok=false;
    if(!service || !iface || !ipv4 || !dns || !values || !primary || !priority) goto done;
    ip_key=SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,kSCDynamicStoreDomainState,service,kSCEntNetIPv4);
    dns_key=SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,kSCDynamicStoreDomainState,service,kSCEntNetDNS);
    if(!ip_key || !dns_key) goto done;

    /* configd needs a routable IPv4 service to elect a primary network and
     * publish NWI/default DNS. A supplemental DNS entity alone depends on
     * another active service and breaks getaddrinfo when that service exits. */
    CFDictionarySetValue(ipv4,kSCPropInterfaceName,iface);
    CFDictionarySetValue(ipv4,kSCPropNetOverridePrimary,primary);
    if(!set_address(ipv4,kSCPropNetIPv4Addresses,lease->ip,true) ||
       !set_address(ipv4,kSCPropNetIPv4SubnetMasks,lease->mask,true) ||
       !set_address(ipv4,kSCPropNetIPv4DestAddresses,lease->gateway,true) ||
       !set_address(ipv4,kSCPropNetIPv4Router,lease->gateway,false)) goto done;

    CFDictionarySetValue(dns,kSCPropInterfaceName,iface);
    CFDictionarySetValue(dns,kSCPropNetDNSSearchOrder,priority);
    if(!set_address(dns,kSCPropNetDNSServerAddresses,lease->dns,true)) goto done;
    CFDictionarySetValue(values,ip_key,ipv4);
    CFDictionarySetValue(values,dns_key,dns);
    ok=true;
done:
    if(service) CFRelease(service);
    if(iface) CFRelease(iface);
    if(ipv4) CFRelease(ipv4);
    if(dns) CFRelease(dns);
    if(primary) CFRelease(primary);
    if(priority) CFRelease(priority);
    if(ip_key) CFRelease(ip_key);
    if(dns_key) CFRelease(dns_key);
    if(!ok && values) { CFRelease(values); values=NULL; }
    return values;
}
