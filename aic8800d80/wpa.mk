# Unmodified WPA2 state machine and internal crypto from upstream 2.11.
WPA_ROOT = vendor/wpa_supplicant
WPA_SOURCES = $(addprefix $(WPA_ROOT)/,$(shell cat $(WPA_ROOT)/SOURCES))
WPA_OBJECTS = $(patsubst $(WPA_ROOT)/%.c,build/wpa/%.o,$(WPA_SOURCES))
WPA_FLAGS = -I$(WPA_ROOT)/src -I$(WPA_ROOT)/src/utils \
	-DCONFIG_NO_RANDOM_POOL -DCONFIG_NO_CONFIG_BLOBS -DCONFIG_NO_RC4 \
	-DCONFIG_NO_TKIP -DCONFIG_SHA256 -DCONFIG_NO_WPA_MSG -DCONFIG_CRYPTO_INTERNAL

build/wpa/%.o: $(WPA_ROOT)/%.c wpa.mk
	mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 -w $(WPA_FLAGS) -c $< -o $@

build/libwpa.a: $(WPA_OBJECTS)
	$(AR) rcs $@ $^
