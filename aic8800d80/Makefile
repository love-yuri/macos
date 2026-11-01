CC = clang
.DEFAULT_GOAL := all
CFLAGS = -std=c11 -O2 -Wall -Wextra -Werror
include wpa.mk
NATIVE_SOURCES = aicusb.c firmware.c scan.c station.c datapath.c auth.c connect.c netstack.c interface.c routing.c service.c credentials.c
NATIVE_HEADERS = $(wildcard *.h)
AIC_ABI_HEADERS = $(wildcard vendor/aic8800/*.h)

.PHONY: all test clean
all: build/aicusb

build:
	mkdir -p $@

build/aicusb: $(NATIVE_SOURCES) $(NATIVE_HEADERS) $(AIC_ABI_HEADERS) Makefile build/libwpa.a | build
	$(CC) $(CFLAGS) $(WPA_FLAGS) -Wno-unused-parameter -Wno-deprecated-declarations $(NATIVE_SOURCES) build/libwpa.a -framework IOKit -framework CoreFoundation -framework CoreServices -framework Security -framework SystemConfiguration -o $@
	/usr/bin/codesign --force --sign - --identifier com.aic8800.aicusb $@

build/test_protocol: test_protocol.c protocol.h | build
	$(CC) $(CFLAGS) test_protocol.c -o $@

build/test_firmware: test_firmware.c firmware.c firmware.h protocol.h | build
	$(CC) $(CFLAGS) test_firmware.c firmware.c -o $@

build/test_scan: test_scan.c scan.c scan.h firmware.h protocol.h lmac_abi.h $(AIC_ABI_HEADERS) | build
	$(CC) $(CFLAGS) test_scan.c scan.c -o $@

build/test_routing: test_routing.c routing.c routing.h | build
	$(CC) $(CFLAGS) test_routing.c routing.c -o $@

build/test_service: test_service.c service.c service.h netstack.h | build
	$(CC) $(CFLAGS) test_service.c service.c -framework CoreFoundation -framework SystemConfiguration -o $@

build/test_datapath: test_datapath.c datapath.c datapath.h protocol.h | build
	$(CC) $(CFLAGS) test_datapath.c datapath.c -o $@

build/test_auth: test_auth.c auth.c auth.h build/libwpa.a | build
	$(CC) $(CFLAGS) $(WPA_FLAGS) -Wno-unused-parameter -Wno-deprecated-declarations test_auth.c auth.c build/libwpa.a -framework CoreServices -o $@

build/test_netstack: test_netstack.c netstack.c netstack.h | build
	$(CC) $(CFLAGS) test_netstack.c netstack.c -o $@

test: build/aicusb build/test_protocol build/test_firmware build/test_scan build/test_routing build/test_service build/test_datapath build/test_auth build/test_netstack
	./build/test_protocol
	./build/test_firmware
	./build/test_scan
	./build/test_routing
	./build/test_service
	./build/test_datapath
	./build/test_auth
	./build/test_netstack
	/bin/zsh ./test_prepare.zsh
	/usr/bin/python3 ./test_autostart.py

clean:
	rm -rf build
