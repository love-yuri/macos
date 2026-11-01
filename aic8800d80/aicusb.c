/* SPDX-License-Identifier: GPL-2.0-only */
/* Native macOS USB bring-up and userspace IPv4 Wi-Fi connection utility. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <mach/mach_error.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "protocol.h"
#include "firmware.h"
#include "scan.h"
#include "lmac_abi.h"
#include "connect.h"
#include "credentials.h"

struct pending_message {
    struct pending_message *next;
    size_t length;
    uint8_t bytes[];
};

struct device {
    io_service_t service;
    uint16_t vendor, product;
    uint32_t location;
    char name[128];
};

struct transport {
    IOUSBDeviceInterface300 **device;
    IOUSBInterfaceInterface300 **interface;
    UInt8 input, output;
    UInt8 message_input, message_output;
    UInt8 data_input, data_output;
    unsigned pending_count, timeout_ms;
    struct pending_message *pending_head, *pending_tail;
    bool captured, device_open;
};

static uint32_t property_number(io_service_t service, CFStringRef key)
{
    CFTypeRef p = IORegistryEntryCreateCFProperty(service, key, NULL, 0);
    int64_t result = 0;
    if (p) {
        if (CFGetTypeID(p) == CFNumberGetTypeID())
            CFNumberGetValue(p, kCFNumberSInt64Type, &result);
        CFRelease(p);
    }
    return (uint32_t)result;
}

static bool supported(const struct device *d)
{
    return (d->vendor == 0x1111 && d->product == 0x1111 &&
            strcmp(d->name, "88M80") == 0) ||
           (d->vendor == 0xa69c &&
            (d->product == 0x8d80 || d->product == 0x8d81));
}

/* Keep only the selected service. Never print serial numbers or Wi-Fi keys. */
static int find_device(uint32_t location, bool listing, struct device *selected)
{
    io_iterator_t iterator = IO_OBJECT_NULL;
    IOReturn kr = IOServiceGetMatchingServices(kIOMainPortDefault,
        IOServiceMatching("IOUSBHostDevice"), &iterator);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "USB enumeration failed: 0x%08x\n", (unsigned)kr);
        return -1;
    }
    int count = 0;
    io_service_t service;
    while ((service = IOIteratorNext(iterator))) {
        struct device d = {.service = service};
        d.vendor = property_number(service, CFSTR("idVendor"));
        d.product = property_number(service, CFSTR("idProduct"));
        d.location = property_number(service, CFSTR("locationID"));
        CFTypeRef name = IORegistryEntryCreateCFProperty(service,
            CFSTR("USB Product Name"), NULL, 0);
        if (name) {
            if (CFGetTypeID(name) == CFStringGetTypeID())
                CFStringGetCString(name, d.name, sizeof(d.name), kCFStringEncodingUTF8);
            CFRelease(name);
        }
        if (supported(&d) && (!location || d.location == location)) {
            count++;
            if (listing)
                printf("%04x:%04x  location=0x%08x  %s  [%s]\n",
                    d.vendor, d.product, d.location, d.name,
                    d.vendor == 0x1111 ? "installation disk / mode switch required" :
                    d.product == 0x8d80 ? "boot ROM / firmware required" :
                    "runtime firmware / use connect for networking");
            if (selected && count == 1) {
                *selected = d;
                IOObjectRetain(service);
            }
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    if (selected && count != 1 && selected->service) {
        IOObjectRelease(selected->service);
        selected->service = IO_OBJECT_NULL;
    }
    return count;
}

static void close_transport(struct transport *t)
{
    while (t->pending_head) {
        struct pending_message *p = t->pending_head;
        t->pending_head = p->next;
        free(p);
    }
    t->pending_tail = NULL;
    t->pending_count = 0;
    if (t->interface) {
        (*t->interface)->USBInterfaceClose(t->interface);
        (*t->interface)->Release(t->interface);
        t->interface = NULL;
    }
    if (t->device) {
        if (t->device_open)
            (*t->device)->USBDeviceClose(t->device);
        if (t->captured) {
            IOReturn kr = (*t->device)->USBDeviceReEnumerate(t->device,
                kUSBReEnumerateReleaseDeviceMask);
            if (kr != kIOReturnSuccess && kr != kIOReturnNoDevice)
                fprintf(stderr, "USB capture release returned 0x%08x; "
                    "unplug/replug this adapter if needed.\n", (unsigned)kr);
        }
        (*t->device)->Release(t->device);
        t->device = NULL;
    }
}

static IOReturn open_interface(struct transport *t, bool storage, const char **stage)
{
    IOUSBFindInterfaceRequest request = {
        .bInterfaceClass = storage ? 8 : 0xff,
        .bInterfaceSubClass = storage ? 6 : 0xff,
        .bInterfaceProtocol = storage ? 0x50 : 0xff,
        .bAlternateSetting = 0
    };
    IOReturn kr=kIOReturnNotFound;
    /* Runtime firmware may publish its device before the interface is ready.
     * Storage plug-in failures can instead be TCC denials; retries do not grant
     * removable-volume permission to a background daemon. */
    unsigned attempts = storage ? 1 : 51;
    for(unsigned attempt=0;attempt<attempts;attempt++) {
        io_iterator_t iterator=IO_OBJECT_NULL;
        *stage="enumerate USB interfaces";
        kr=(*t->device)->CreateInterfaceIterator(t->device,&request,&iterator);
        if(kr!=kIOReturnSuccess) return kr;
        io_service_t service=IOIteratorNext(iterator);
        IOObjectRelease(iterator);
        if(!service) {
            *stage="find matching USB interface";
            kr=kIOReturnNotFound;
        } else {
            IOCFPlugInInterface **plugin=NULL;
            SInt32 score=0;
            *stage="create USB interface plug-in";
            kr=IOCreatePlugInInterfaceForService(service,kIOUSBInterfaceUserClientTypeID,
                kIOCFPlugInInterfaceID,&plugin,&score);
            IOObjectRelease(service);
            if(kr==kIOReturnSuccess) {
                *stage="query USB interface";
                HRESULT hr=(*plugin)->QueryInterface(plugin,
                    CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID300),(LPVOID *)&t->interface);
                IODestroyPlugInInterface(plugin);
                if(hr || !t->interface) return kIOReturnUnsupported;
                *stage="open USB interface";
                kr=storage ? (*t->interface)->USBInterfaceOpenSeize(t->interface) :
                    (*t->interface)->USBInterfaceOpen(t->interface);
                if(kr==kIOReturnSuccess) return kr;
                (*t->interface)->Release(t->interface);
                t->interface=NULL;
            }
        }
        if(kr!=kIOReturnNoResources && kr!=kIOReturnNotFound && kr!=kIOReturnNoDevice &&
           kr!=kIOReturnNotReady && kr!=kIOReturnBusy && kr!=kIOReturnExclusiveAccess)
            return kr;
        if(!attempt) fprintf(stderr,"Waiting for USB interface readiness (%s, 0x%08x)...\n",*stage,(unsigned)kr);
        if(attempt+1<attempts) usleep(100000);
    }
    return kr;
}

static IOReturn open_transport(const struct device *d, bool storage,
                               struct transport *t)
{
    /* Mass storage is held by IOUSBMassStorageInterfaceNub. A plain
     * USBInterfaceOpenSeize cannot detach it. Apple's USB capture API needs
     * root, or a separately granted entitlement not used by this CLI.
     */
    if (storage && geteuid() != 0)
        return kIOReturnNotPrivileged;
    IOCFPlugInInterface **plugin = NULL;
    IOUSBDeviceInterface300 **usb = NULL;
    SInt32 score = 0;
    IOReturn kr = IOCreatePlugInInterfaceForService(d->service,
        kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin, &score);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr,"USB open stage: create device plug-in.\n");
        return kr;
    }
    HRESULT hr = (*plugin)->QueryInterface(plugin,
        CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID300), (LPVOID *)&usb);
    IODestroyPlugInInterface(plugin);
    if (hr || !usb) {
        fprintf(stderr,"USB open stage: query device interface (%08x).\n",(unsigned)hr);
        return kIOReturnUnsupported;
    }
    t->device = usb;
    const char *stage="capture installation-disk device";
    if (storage) {
        kr = (*usb)->USBDeviceReEnumerate(usb, kUSBReEnumerateCaptureDeviceMask);
        if (kr != kIOReturnSuccess)
            goto fail;
        t->captured = true;
        stage="open captured device";
        kr = (*usb)->USBDeviceOpenSeize(usb);
        if (kr != kIOReturnSuccess)
            goto fail;
        t->device_open = true;
    }
    kr=open_interface(t,storage,&stage);
    if(kr!=kIOReturnSuccess) goto fail;
    UInt8 endpoints = 0;
    stage="read USB endpoints";
    kr = (*t->interface)->GetNumEndpoints(t->interface, &endpoints);
    if (kr != kIOReturnSuccess)
        goto fail;
    for (unsigned pipe = 1; pipe <= endpoints; pipe++) {
        UInt8 direction, number, type, interval;
        UInt16 packet;
        kr = (*t->interface)->GetPipeProperties(t->interface, (UInt8)pipe,
            &direction, &number, &type, &packet, &interval);
        if (kr != kIOReturnSuccess)
            goto fail;
        printf("  pipe=%u endpoint=0x%02x type=%u max_packet=%u\n",
            pipe, number | (direction == kUSBIn ? 0x80 : 0), type, packet);
        if (type == kUSBBulk) {
            if (direction == kUSBIn) {
                if (!t->input)
                    t->input = (UInt8)pipe;
                else if (!t->message_input)
                    t->message_input = (UInt8)pipe;
            }
            if (direction == kUSBOut) {
                if (!t->output)
                    t->output = (UInt8)pipe;
                else if (!t->message_output)
                    t->message_output = (UInt8)pipe;
            }
        }
    }
    /* Runtime uses the second bulk pair for commands, per aicwf_parse_usb(). */
    if (d->product == 0x8d81) {
        t->data_input = t->input;
        t->data_output = t->output;
        if (t->message_input)
            t->input = t->message_input;
        if (t->message_output)
            t->output = t->message_output;
    }
    if (t->input && t->output)
        return kIOReturnSuccess;
    kr = kIOReturnNotFound;
fail:
    fprintf(stderr,"USB open stage: %s (0x%08x).\n",stage,(unsigned)kr);
    close_transport(t);
    return kr;
}

static int usb_error(const char *operation, IOReturn kr)
{
    fprintf(stderr, "%s: 0x%08x (%s)\n", operation, (unsigned)kr,
        mach_error_string((mach_error_t)kr));
    if (kr == kIOReturnNotPermitted || kr == kIOReturnNotPrivileged ||
        kr == kIOReturnExclusiveAccess)
        fprintf(stderr, "USB access was denied or the interface is owned. "
            "Run this exact command in a local Terminal with sudo if required.\n");
    return 1;
}

static int switch_mode(const struct device *d)
{
    if (d->vendor != 0x1111 || d->product != 0x1111) {
        fprintf(stderr, "Device is already past installation-disk mode; no command sent.\n");
        return 0;
    }
    struct transport t = {0};
    IOReturn kr = open_transport(d, true, &t);
    if (kr != kIOReturnSuccess) {
        if (kr == kIOReturnNoResources || kr == kIOReturnNotPermitted ||
            kr == kIOReturnNotPrivileged)
            fprintf(stderr, "If system logs report kTCCServiceSystemPolicyRemovableVolumes denied, "
                "authorize this executable in macOS Privacy & Security settings. "
                "Administrator authentication alone does not grant removable-volume access.\n");
        return usb_error("Open installation-disk interface", kr);
    }
    for (unsigned i = 0; i < 2; i++) {
        uint8_t cbw[31], csw[13] = {0};
        aic_switch_cbw(cbw, i == 0 ? 0xf3 : 0xf2);
        printf("Sending vendor mode-switch command F%u...\n", i == 0 ? 3 : 2);
        kr = (*t.interface)->WritePipeTO(t.interface, t.output, cbw, sizeof(cbw), 1000, 2000);
        /* The final command may disconnect the device before its completion. */
        if (kr != kIOReturnSuccess) {
            if (i == 1)
                fprintf(stderr, "Final write returned 0x%08x; checking re-enumeration.\n", (unsigned)kr);
            else {
                close_transport(&t);
                return usb_error("F3 write", kr);
            }
            break;
        }
        UInt32 size = sizeof(csw);
        kr = (*t.interface)->ReadPipeTO(t.interface, t.input, csw, &size, 1000, 2000);
        if (kr == kIOReturnSuccess) {
            printf("  F%u response (%u bytes):", i == 0 ? 3 : 2, size);
            for (unsigned j = 0; j < size && j < sizeof(csw); j++)
                printf(" %02x", csw[j]);
            putchar('\n');
            /* usb_modeswitch 2.6.2 switchSendMessage() logs the returned
             * SCSI status but does not require a successful CSW. These are
             * vendor mode-switch commands, not a storage read/write.
             * Follow the same F3 -> F2 sequence on successful USB transport;
             * only observing the new identity below proves the switch.
             */
            if (!aic_valid_csw(csw, size))
                puts("  Vendor response is not a successful standard CSW; "
                     "continuing the repository's F3 -> F2 sequence as usb_modeswitch does.");
        }
        if (i == 0 && kr != kIOReturnSuccess) {
            close_transport(&t);
            return usb_error("F3 status read", kr);
        }
    }
    close_transport(&t);
    for (unsigned attempt = 0; attempt < 25; attempt++) {
        usleep(200000);
        struct device next = {0};
        if (find_device(d->location, false, &next) == 1) {
            bool switched = next.vendor == 0xa69c;
            if (switched)
                printf("Verified mode switch: %04x:%04x at 0x%08x. "
                    "This alone does not enable Wi-Fi.\n", next.vendor, next.product, next.location);
            IOObjectRelease(next.service);
            if (switched)
                return 0;
        }
    }
    fprintf(stderr, "Mode switch was not verified at this USB port. Run 'list' again.\n");
    return 1;
}

static int chip_info(const struct device *d)
{
    if (d->vendor != 0xa69c || d->product != 0x8d80) {
        fprintf(stderr, "chip-info only supports A69C:8D80 boot ROM. "
            "Use 'switch' first if the adapter is 1111:1111.\n");
        return 1;
    }
    struct transport t = {0};
    IOReturn kr = open_transport(d, false, &t);
    if (kr != kIOReturnSuccess)
        return usb_error("Open boot-ROM interface", kr);
    uint8_t request[20];
    aic_chip_read_request(request);
    kr = (*t.interface)->WritePipeTO(t.interface, t.output, request, sizeof(request), 1000, 2000);
    if (kr != kIOReturnSuccess) {
        close_transport(&t);
        return usb_error("Chip-register read request", kr);
    }
    for (unsigned attempt = 0; attempt < 4; attempt++) {
        uint8_t response[2048];
        UInt32 size = sizeof(response);
        kr = (*t.interface)->ReadPipeTO(t.interface, t.input, response, &size, 1000, 2000);
        if (kr != kIOReturnSuccess) {
            close_transport(&t);
            return usb_error("Chip-register confirmation", kr);
        }
        uint32_t value = 0;
        int result = aic_parse_chip_response(response, size, &value);
        if (result == 1) {
            unsigned revision = (value >> 16) & 0xff;
            unsigned mcu = ((value >> 25) & 1) == 0;
            printf("register[0x40500000]=0x%08x chip_id=%u chip_mcu_id=%u\n",
                value, revision, mcu);
            printf("Linux firmware reference branch: %s\n", mcu ? "legacy-mcu1" : "main");
            close_transport(&t);
            return 0;
        }
        fprintf(stderr, "%s USB response (%u bytes):", result < 0 ? "Malformed" : "Other", size);
        for (unsigned j = 0; j < size && j < 64; j++)
            fprintf(stderr, " %02x", response[j]);
        fputc('\n', stderr);
        if (result < 0)
            break;
    }
    close_transport(&t);
    fprintf(stderr, "No valid chip confirmation; no firmware has been uploaded.\n");
    return 1;
}

static int debug_exchange(void *context, uint16_t id, const uint8_t *params,
                           size_t length, uint8_t *confirmation,
                           size_t capacity, size_t *actual)
{
    struct transport *t = context;
    uint8_t request[1048];
    size_t size = aic_debug_request(request, sizeof(request), id, params, length);
    if (!size)
        return -1;
    IOReturn kr = (*t->interface)->WritePipeTO(t->interface, t->output,
        request, (UInt32)size, 1000, 2000);
    if (kr != kIOReturnSuccess) {
        usb_error("Firmware command write", kr);
        return -1;
    }
    if (id == 0x40d)
        return 0; /* Linux sends DBG_START_APP_REQ without waiting for CFM. */
    for (unsigned i = 0; i < 128; i++) {
        uint8_t response[8192];
        UInt32 received = sizeof(response);
        kr = (*t->interface)->ReadPipeTO(t->interface, t->input,
            response, &received, 1000, 2000);
        if (kr != kIOReturnSuccess) {
            fprintf(stderr, "Waiting for command 0x%04x confirmation: ", id);
            usb_error("Firmware command read", kr);
            return -1;
        }
        uint16_t expected_id = id == SCANU_START_REQ ? SCANU_START_CFM_ADDTIONAL : id + 1;
        bool matched = false;
        for (size_t off = 0; off < received;) {
            if (received-off < 4) return -1;
            size_t payload = aic_get16(response+off);
            size_t frame = 4 + ((payload+3) & ~(size_t)3);
            if (!payload || frame > received-off) return -1;
            int parsed = aic_parse_debug_cfm(response+off, frame, expected_id,
                confirmation, capacity, actual);
            if (parsed < 0) return -1;
            if (parsed == 1) matched = true;
            else {
                if (t->pending_count >= 256) {
                    fprintf(stderr, "Firmware event queue overflow.\n");
                    return -1;
                }
                struct pending_message *p = malloc(sizeof(*p)+frame);
                if (!p) return -1;
                p->next = NULL; p->length = frame;
                memcpy(p->bytes, response+off, frame);
                if (t->pending_tail) t->pending_tail->next = p;
                else t->pending_head = p;
                t->pending_tail = p;
                t->pending_count++;
            }
            off += frame;
        }
        if (matched) return 0;
    }
    return -1;
}

static int load_firmware(const struct device *d, const char *directory)
{
    if (d->vendor != 0xa69c || d->product != 0x8d80) {
        fprintf(stderr, "load-fw requires the A69C:8D80 boot-ROM identity.\n");
        return 1;
    }
    struct transport t = {0};
    IOReturn kr = open_transport(d, false, &t);
    if (kr != kIOReturnSuccess)
        return usb_error("Open boot-ROM interface", kr);
    int result = aic_firmware_boot(directory, debug_exchange, &t);
    close_transport(&t);
    if (result) {
        fprintf(stderr, "Firmware bring-up failed. Unplug/replug the adapter before retrying.\n");
        return 1;
    }
    for (unsigned i = 0; i < 25; i++) {
        usleep(200000);
        struct device next = {0};
        if (find_device(d->location, false, &next) == 1) {
            bool runtime = next.vendor == 0xa69c && next.product == 0x8d81;
            IOObjectRelease(next.service);
            if (runtime) {
                puts("Verified RAM firmware startup: A69C:8D81. "
                     "Use connect to authenticate and start macOS networking.");
                return 0;
            }
        }
    }
    fprintf(stderr, "RAM firmware was sent, but runtime USB identity was not verified.\n");
    return 1;
}

static int runtime_info(const struct device *d)
{
    if (d->vendor != 0xa69c || d->product != 0x8d81) {
        fprintf(stderr, "runtime-info requires A69C:8D81. Load firmware first.\n");
        return 1;
    }
    struct transport t = {0};
    IOReturn kr = open_transport(d, false, &t);
    if (kr != kIOReturnSuccess)
        return usb_error("Open runtime WLAN interface", kr);
    uint8_t version[64];
    size_t actual = 0;
    int result = debug_exchange(&t, 4, NULL, 0, version, sizeof(version), &actual);
    close_transport(&t);
    if (result || actual < 27) {
        fprintf(stderr, "No valid MM_VERSION_CFM from running firmware.\n");
        return 1;
    }
    printf("Running LMAC version=0x%08x features=0x%08x max_stations=%u max_interfaces=%u\n",
        aic_get32(version), aic_get32(version + 20), aic_get16(version + 24), version[26]);
    return 0;
}

static int receive_message(void *context, uint8_t *buffer, size_t capacity, size_t *actual)
{
    struct transport *t = context;
    *actual = 0;
    if (t->pending_head) {
        struct pending_message *p = t->pending_head;
        if (p->length > capacity) return -1;
        memcpy(buffer, p->bytes, p->length);
        *actual = p->length;
        t->pending_head = p->next;
        if (!t->pending_head) t->pending_tail = NULL;
        t->pending_count--;
        free(p);
        return 1;
    }
    UInt32 size = (UInt32)capacity;
    unsigned timeout = t->timeout_ms ? t->timeout_ms : 200;
    IOReturn kr = (*t->interface)->ReadPipeTO(t->interface, t->input, buffer, &size, timeout, timeout);
    *actual = size;
    if (kr == kIOUSBTransactionTimeout || kr == kIOReturnTimeout)
        return 0;
    if (kr != kIOReturnSuccess) {
        usb_error("Firmware event read", kr);
        return -1;
    }
    return 1;
}

static int receive_data(void *context, uint8_t *buffer, size_t capacity, size_t *actual)
{
    struct transport *t = context;
    UInt32 size = (UInt32)capacity;
    IOReturn kr = (*t->interface)->ReadPipeTO(t->interface, t->data_input,
        buffer, &size, 5, 5);
    *actual = kr == kIOReturnSuccess ? size : 0;
    if (kr == kIOUSBTransactionTimeout || kr == kIOReturnTimeout) {
        if(size>0 && size<capacity) { *actual=size; return 1; }
        return 0;
    }
    if (kr != kIOReturnSuccess) { usb_error("USB data read", kr); return -1; }
    return 1;
}

static int send_data(void *context, const uint8_t *buffer, size_t length)
{
    struct transport *t = context;
    IOReturn kr = (*t->interface)->WritePipeTO(t->interface, t->data_output,
        (void *)buffer, (UInt32)length, 1000, 2000);
    if (kr != kIOReturnSuccess) { usb_error("USB data write", kr); return -1; }
    return 0;
}

static int connect_network(const struct device *d, const char *ssid, bool route, bool probe, unsigned duration,
                           const char *credential_file)
{
    if (d->vendor != 0xa69c || d->product != 0x8d81) {
        fprintf(stderr, "connect requires running A69C:8D81 firmware.\n"); return 1;
    }
    struct transport t = {0};
    IOReturn kr = open_transport(d, false, &t);
    if (kr != kIOReturnSuccess) return usb_error("Open runtime WLAN interface", kr);
    const struct aic_connection_io io = {.context = &t, .exchange = debug_exchange,
        .receive = receive_message, .receive_data = receive_data, .send_data = send_data};
    t.timeout_ms = 10;
    int result = aic_connect(&io, ssid, route, probe, duration, credential_file);
    close_transport(&t);
    return result ? 1 : 0;
}

static int scan_networks(const struct device *d)
{
    if (d->vendor != 0xa69c || d->product != 0x8d81) {
        fprintf(stderr, "scan requires running A69C:8D81 firmware.\n");
        return 1;
    }
    struct transport t = {0};
    IOReturn kr = open_transport(d, false, &t);
    if (kr != kIOReturnSuccess)
        return usb_error("Open runtime WLAN interface", kr);
    int result = aic_passive_scan(&t, debug_exchange, receive_message);
    close_transport(&t);
    return result ? 1 : 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2 || argc % 2 != 0 ||
        (strcmp(argv[1], "list") && strcmp(argv[1], "switch") && strcmp(argv[1], "chip-info") &&
         strcmp(argv[1], "load-fw") && strcmp(argv[1], "check-fw") &&
         strcmp(argv[1], "runtime-info") && strcmp(argv[1], "scan") && strcmp(argv[1], "connect") &&
         strcmp(argv[1], "save-credential") && strcmp(argv[1], "check-credential"))) {
        fprintf(stderr, "Usage: %s list|switch|chip-info|load-fw|check-fw|runtime-info|scan "
            "[--location 0x00b00000] [--firmware-dir PATH]\n"
            "       %s connect --ssid NAME [--location ID] [--default-route yes|no] [--probe yes|no] [--duration SECONDS] [--credential-file PATH]\n"
            "       %s save-credential|check-credential --ssid NAME --credential-file PATH\n", argv[0], argv[0], argv[0]);
        return 2;
    }
    uint32_t location = 0;
    const char *directory = NULL;
    const char *ssid = NULL;
    const char *credential_file = NULL;
    bool save_credential = !strcmp(argv[1], "save-credential");
    bool check_credential = !strcmp(argv[1], "check-credential");
    bool route = false, probe = false;
    unsigned duration = 0;
    for (int i = 2; i < argc; i += 2) {
        if (!strcmp(argv[1], "connect") && !strcmp(argv[i], "--duration")) {
            char *end; errno=0; unsigned long value=strtoul(argv[i+1],&end,10);
            if(errno || *end || end==argv[i+1] || value>86400) return 2;
            duration=(unsigned)value; continue;
        }
        if ((!strcmp(argv[1], "connect") || save_credential || check_credential) && !strcmp(argv[i], "--ssid") && !ssid) {
            ssid = argv[i+1]; continue;
        }
        if ((!strcmp(argv[1], "connect") || save_credential || check_credential) && !strcmp(argv[i], "--credential-file") && !credential_file) {
            credential_file=argv[i+1]; continue;
        }
        if (!strcmp(argv[1], "connect") &&
            (!strcmp(argv[i], "--default-route") || !strcmp(argv[i], "--probe"))) {
            if (strcmp(argv[i+1], "yes") && strcmp(argv[i+1], "no")) return 2;
            if (!strcmp(argv[i], "--probe")) probe = !strcmp(argv[i+1], "yes");
            else route = !strcmp(argv[i+1], "yes");
            continue;
        }
        if (strcmp(argv[i], "--firmware-dir") == 0 && !directory) {
            directory = argv[i + 1];
            continue;
        }
        if (strcmp(argv[i], "--location") || location) {
            fprintf(stderr, "Unknown or repeated option: %s\n", argv[i]);
            return 2;
        }
        char *end;
        errno = 0;
        unsigned long parsed = strtoul(argv[i + 1], &end, 0);
        if (errno || *end || end == argv[i + 1] || !parsed || parsed > UINT32_MAX) {
            fprintf(stderr, "Invalid USB location.\n");
            return 2;
        }
        location = (uint32_t)parsed;
    }
    bool firmware = strcmp(argv[1], "load-fw") == 0;
    if ((!strcmp(argv[1], "connect") || save_credential || check_credential) && (!ssid || !strlen(ssid) || strlen(ssid)>32)) {
        fprintf(stderr, "connect requires --ssid with 1-32 bytes.\n"); return 2;
    }
    if(save_credential || check_credential) {
        if(!credential_file || directory || location) return 2;
        if(save_credential) return aic_save_password(credential_file,ssid)?1:0;
        char password[256]={0};
        int result=aic_read_password_file(credential_file,ssid,password,sizeof(password));
        aic_clear(password,sizeof(password));
        if(!result) puts("Unattended Wi-Fi credential and permissions verified (no USB access).");
        return result?1:0;
    }
    bool check = strcmp(argv[1], "check-fw") == 0;
    if ((firmware || check) != (directory != NULL) || (check && location)) {
        fprintf(stderr, "load-fw/check-fw require --firmware-dir; "
            "other commands do not accept it. check-fw does not use a USB location.\n");
        return 2;
    }
    if (check)
        return aic_firmware_boot(directory, NULL, NULL) ? 1 : 0;
    bool listing = strcmp(argv[1], "list") == 0;
    struct device d = {0};
    int count = find_device(location, listing, listing ? NULL : &d);
    if (count <= 0) {
        fprintf(stderr, "No supported AIC 88M80 device found (or USB access is restricted).\n");
        return 1;
    }
    if (listing)
        return 0;
    if (count != 1) {
        fprintf(stderr, "Multiple devices found. Select one with --location.\n");
        return 1;
    }
    printf("Selected %04x:%04x %s at 0x%08x\n", d.vendor, d.product, d.name, d.location);
    int result = firmware ? load_firmware(&d, directory) :
        strcmp(argv[1], "connect") == 0 ? connect_network(&d, ssid, route, probe, duration, credential_file) :
        strcmp(argv[1], "scan") == 0 ? scan_networks(&d) :
        strcmp(argv[1], "runtime-info") == 0 ? runtime_info(&d) :
        strcmp(argv[1], "switch") == 0 ? switch_mode(&d) : chip_info(&d);
    IOObjectRelease(d.service);
    return result;
}
