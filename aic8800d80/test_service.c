/* SPDX-License-Identifier: GPL-2.0-only */
#include "service.h"
#include <SystemConfiguration/SystemConfiguration.h>
#include <assert.h>
#include <stdio.h>

static CFTypeRef first(CFDictionaryRef dict, CFStringRef key)
{
    CFArrayRef array=CFDictionaryGetValue(dict,key);
    assert(array && CFGetTypeID(array)==CFArrayGetTypeID() && CFArrayGetCount(array)==1);
    return CFArrayGetValueAtIndex(array,0);
}
int main(void)
{
    struct aic_lease lease={.ip={192,168,1,3},.mask={255,255,255,0},
        .gateway={192,168,1,1},.dns={192,168,1,1}};
    /* No pre-existing service or resolver is supplied: AIC must be a complete
     * primary-network candidate, not a DNS-only supplement to another NIC. */
    CFDictionaryRef values=aic_service_values_create("AIC-test","utun42",&lease);
    assert(values && CFDictionaryGetCount(values)==2);
    CFDictionaryRef ipv4=CFDictionaryGetValue(values,CFSTR("State:/Network/Service/AIC-test/IPv4"));
    CFDictionaryRef dns=CFDictionaryGetValue(values,CFSTR("State:/Network/Service/AIC-test/DNS"));
    assert(ipv4 && dns);
    assert(CFEqual(CFDictionaryGetValue(ipv4,kSCPropInterfaceName),CFSTR("utun42")));
    assert(CFEqual(first(ipv4,kSCPropNetIPv4Addresses),CFSTR("192.168.1.3")));
    assert(CFEqual(first(ipv4,kSCPropNetIPv4SubnetMasks),CFSTR("255.255.255.0")));
    assert(CFEqual(first(ipv4,kSCPropNetIPv4DestAddresses),CFSTR("192.168.1.1")));
    assert(CFEqual(CFDictionaryGetValue(ipv4,kSCPropNetIPv4Router),CFSTR("192.168.1.1")));
    int override=0;
    assert(CFNumberGetValue(CFDictionaryGetValue(ipv4,kSCPropNetOverridePrimary),kCFNumberIntType,&override) && override==1);
    assert(CFEqual(CFDictionaryGetValue(dns,kSCPropInterfaceName),CFSTR("utun42")));
    assert(CFEqual(first(dns,kSCPropNetDNSServerAddresses),CFSTR("192.168.1.1")));
    assert(!CFDictionaryContainsKey(dns,kSCPropNetDNSSupplementalMatchDomains));
    assert(!CFDictionaryContainsKey(values,CFSTR("State:/Network/Global/IPv4")));
    CFRelease(values);

    lease.ip[3]=9; lease.dns[3]=53;
    values=aic_service_values_create("AIC-test","utun42",&lease);
    assert(values);
    ipv4=CFDictionaryGetValue(values,CFSTR("State:/Network/Service/AIC-test/IPv4"));
    dns=CFDictionaryGetValue(values,CFSTR("State:/Network/Service/AIC-test/DNS"));
    assert(CFEqual(first(ipv4,kSCPropNetIPv4Addresses),CFSTR("192.168.1.9")));
    assert(CFEqual(first(dns,kSCPropNetDNSServerAddresses),CFSTR("192.168.1.53")));
    CFRelease(values);
    assert(!aic_service_values_create("","utun42",&lease));
    assert(!aic_service_values_create("AIC-test","",&lease));
    assert(!aic_service_values_create("AIC-test","utun42",NULL));
    puts("Network service tests passed: paired IPv4/default DNS, primary eligibility without another NIC, lease refresh, session-only keys (no live system changes).");
    return 0;
}
