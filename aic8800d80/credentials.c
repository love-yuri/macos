/* SPDX-License-Identifier: GPL-2.0-only */
#include "credentials.h"
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <readpassphrase.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

static int valid_password(const char *password)
{
    size_t n=strlen(password);
    if(n>=8 && n<=63) return 1;
    if(n!=64) return 0;
    return strspn(password,"0123456789abcdefABCDEF")==64;
}

int aic_read_password_file(const char *path, const char *ssid, char *out, size_t capacity)
{
    /* Reject symlinks, foreign owners and any group/other access. The daemon
     * uses a root-owned directory, so its path cannot be replaced by a user. */
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    struct stat st;
    uint8_t data[107]={0};
    size_t used=0;
    int result=-1;
    if(fd<0) goto done;
    if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=0 ||
       (st.st_mode&077) || st.st_nlink!=1 || st.st_size<10 || st.st_size>106) goto done;
    while(used<sizeof(data)) {
        ssize_t n=read(fd,data+used,sizeof(data)-used);
        if(n<0 && errno==EINTR) continue;
        if(n<0) goto done;
        if(!n) break;
        used+=(size_t)n;
    }
    size_t sn=data[8], pn=data[9];
    if(used!=10+sn+pn || memcmp(data,"AICWIFI1",8) || sn<1 || sn>32 || pn>64 ||
       pn>=capacity || strlen(ssid)!=sn || memcmp(data+10,ssid,sn) ||
       memchr(data+10+sn,0,pn)) goto done;
    memcpy(out,data+10+sn,pn); out[pn]=0;
    if(!valid_password(out)) { aic_clear(out,capacity); goto done; }
    result=0;
done:
    if(fd>=0) close(fd);
    aic_clear(data,sizeof(data));
    if(result) {
        aic_clear(out,capacity);
        fprintf(stderr,"Cannot read root-only Wi-Fi credential for this SSID; reinstall autostart to configure it.\n");
    }
    return result;
}

int aic_save_password(const char *path, const char *ssid)
{
    size_t sn=strlen(ssid);
    if(geteuid()!=0 || !sn || sn>32) {
        fprintf(stderr,"Saving an unattended credential requires root and a valid SSID.\n"); return -1;
    }
    char password[256]={0};
    if(aic_read_password(ssid,password,sizeof(password)) || !valid_password(password)) {
        aic_clear(password,sizeof(password));
        fprintf(stderr,"No valid WPA2 credential was saved.\n"); return -1;
    }
    uint8_t data[106]={0};
    size_t pn=strlen(password), length=10+sn+pn;
    memcpy(data,"AICWIFI1",8); data[8]=(uint8_t)sn; data[9]=(uint8_t)pn;
    memcpy(data+10,ssid,sn); memcpy(data+10+sn,password,pn);
    aic_clear(password,sizeof(password));
    char *temporary=malloc(strlen(path)+12);
    if(!temporary) { aic_clear(data,sizeof(data)); return -1; }
    sprintf(temporary,"%s.XXXXXX",path);
    int fd=mkstemp(temporary), result=-1;
    if(fd>=0) {
        size_t written=0;
        while(written<length) {
            ssize_t n=write(fd,data+written,length-written);
            if(n<0 && errno==EINTR) continue;
            if(n<=0) break;
            written+=(size_t)n;
        }
        bool ok=written==length && !fchmod(fd,0600) && !fsync(fd);
        if(close(fd)) ok=false;
        if(ok && !rename(temporary,path)) result=0;
        if(result) unlink(temporary);
    }
    aic_clear(data,sizeof(data)); free(temporary);
    if(result) perror("Save Wi-Fi credential");
    else puts("Saved this network's credential with root-only permissions for unattended boot.");
    return result;
}

int aic_read_password(const char *ssid, char *out, size_t capacity)
{
    /* Exact network only. This reader never prints a password or enumerates
     * other networks. Only the explicit save-credential command persists it. */
    CFStringRef name=CFStringCreateWithCString(NULL,ssid,kCFStringEncodingUTF8);
    if(!name) return -1;
    const void *keys[]={kSecClass,kSecAttrAccount,kSecAttrDescription,kSecReturnData,kSecMatchLimit};
    const void *vals[]={kSecClassGenericPassword,name,CFSTR("AirPort network password"),kCFBooleanTrue,kSecMatchLimitOne};
    CFDictionaryRef query=CFDictionaryCreate(NULL,keys,vals,5,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    CFTypeRef result=NULL;
    OSStatus status=SecItemCopyMatching(query,&result); CFRelease(query);
    if(status==errSecSuccess && result && CFGetTypeID(result)==CFDataGetTypeID()) {
        CFIndex len=CFDataGetLength(result);
        if(len>0 && (size_t)len<capacity) {
            memcpy(out,CFDataGetBytePtr(result),len); out[len]=0;
            CFRelease(result); CFRelease(name); puts("Using this network's saved Wi-Fi credential."); return 0;
        }
    }
    if(result) CFRelease(result);
    if(isatty(STDIN_FILENO)) {
        CFRelease(name);
        return readpassphrase("Wi-Fi password (hidden): ",out,capacity,RPP_REQUIRE_TTY)?0:-1;
    }
    CFStringRef message=CFStringCreateWithFormat(NULL,NULL,CFSTR("输入 Wi-Fi「%@」的密码。仅用于本次连接，不保存。"),name);
    const void *dk[]={kCFUserNotificationAlertHeaderKey,kCFUserNotificationAlertMessageKey,
        kCFUserNotificationTextFieldTitlesKey,kCFUserNotificationDefaultButtonTitleKey,kCFUserNotificationAlternateButtonTitleKey};
    const void *titles[]={CFSTR("Wi-Fi 密码")};
    CFArrayRef fields=CFArrayCreate(NULL,titles,1,&kCFTypeArrayCallBacks);
    const void *dv[]={CFSTR("AIC8800 原生连接"),message,fields,CFSTR("连接"),CFSTR("取消")};
    CFDictionaryRef dict=CFDictionaryCreate(NULL,dk,dv,5,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    SInt32 error=0;
    CFUserNotificationRef note=CFUserNotificationCreate(NULL,180,
        kCFUserNotificationPlainAlertLevel|CFUserNotificationSecureTextField(0),&error,dict);
    CFRelease(dict); CFRelease(fields); CFRelease(message); CFRelease(name);
    if(!note) { fprintf(stderr,"No secure password dialog available; run in a local Terminal.\n"); return -1; }
    CFOptionFlags response=0;
    SInt32 rc=CFUserNotificationReceiveResponse(note,180,&response);
    CFStringRef password=CFUserNotificationGetResponseValue(note,kCFUserNotificationTextFieldValuesKey,0);
    bool ok=!rc && (response&3)==kCFUserNotificationDefaultResponse && password &&
        CFStringGetCString(password,out,capacity,kCFStringEncodingUTF8);
    CFRelease(note);
    if(!ok) fprintf(stderr,"Wi-Fi password input cancelled or unavailable.\n");
    return ok?0:-1;
}
