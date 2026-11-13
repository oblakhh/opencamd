# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
#
# camd build. Cross-compile for the device by setting CC to the rsdk toolchain
# (see tools/build_camd.sh). `make test` always uses the host compiler.

CIVET   := ../third_party/civetweb
CJSON   := ../third_party/cjson
MBEDTLS := ../third_party/mbedtls
CC      ?= cc
HOSTCC  ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
CPPFLAGS := -I$(CIVET)/include -I$(CJSON) -I$(MBEDTLS)/include -DHAVE_TLS -DNO_SSL -DNO_CGI -DUSE_IPV6=0 -DNO_CACHING
# Device libs are built out of tree by tools/build_camd.sh.
MBEDTLS_BUILD ?= ../../build/mbedtls
MBEDTLS_LIBS := $(MBEDTLS_BUILD)/library/libmbedtls.a $(MBEDTLS_BUILD)/library/libmbedx509.a $(MBEDTLS_BUILD)/library/libmbedcrypto.a
LDLIBS  := -lpthread -lcrypt -lrt
# Development-build suffix for the version string (see version.h); empty for releases.
CAMD_BUILD ?=
ifneq ($(CAMD_BUILD),)
CPPFLAGS += -DCAMD_BUILD='"$(CAMD_BUILD)"'
endif

OBJS := main.o capture.o http.o framebuf.o config.o config_store.o flash.o \
        ota.o mjpeg.o ntp.o uploader.o gpio.o netcfg.o wifi.o wpa.o wpa_drv.o net.o crc32.o auth.o log.o cjson.o civetweb.o

all: camd getmac

camd: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(MBEDTLS_LIBS) $(LDLIBS)

getmac: getmac.c
	$(CC) $(CFLAGS) -o $@ getmac.c

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

civetweb.o: $(CIVET)/src/civetweb.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

cjson.o: $(CJSON)/cJSON.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

# Host unit tests (pure-logic modules only; no device needed).
test: tests/test_framebuf tests/test_config tests/test_ota tests/test_mjpeg tests/test_netcfg tests/test_wifi tests/test_wpa
	./tests/test_framebuf
	./tests/test_config
	./tests/test_ota
	./tests/test_mjpeg
	./tests/test_netcfg
	./tests/test_wifi
	./tests/test_wpa

tests/test_framebuf: tests/test_framebuf.c framebuf.c framebuf.h
	$(HOSTCC) -O2 -Wall -Wextra -pthread -I. -o $@ tests/test_framebuf.c framebuf.c

tests/test_config: tests/test_config.c config_store.c config.c flash.c crc32.c $(CJSON)/cJSON.c
	$(HOSTCC) -O2 -Wall -Wextra -I. -I$(CJSON) -o $@ $^

tests/test_ota: tests/test_ota.c ota.c flash.c log.c
	$(HOSTCC) -O2 -Wall -Wextra -I. -o $@ $^

tests/test_netcfg: tests/test_netcfg.c netcfg.c log.c
	$(HOSTCC) -O2 -Wall -Wextra -pthread -I. -o $@ $^

tests/test_wifi: tests/test_wifi.c wifi.c wpa_drv.c wpa.c log.c $(CJSON)/cJSON.c tests/libmbedcrypto-host.a
	$(HOSTCC) -O2 -Wall -Wextra -pthread -I. -I$(CJSON) -I$(MBEDTLS)/include -o $@ $^

# mbedTLS compiled for the host (the device libs in $(MBEDTLS_BUILD) are cross-compiled)
HOST_MBED_OBJS := $(patsubst $(MBEDTLS)/library/%.c,tests/host-mbedtls/%.o,$(wildcard $(MBEDTLS)/library/*.c))
tests/host-mbedtls/%.o: $(MBEDTLS)/library/%.c
	@mkdir -p tests/host-mbedtls
	$(HOSTCC) -O2 -I$(MBEDTLS)/include -c -o $@ $<
tests/libmbedcrypto-host.a: $(HOST_MBED_OBJS)
	rm -f $@; ar rcs $@ $^

# -std=gnu89 + -Wdeclaration-after-statement: the device compiler is gcc 3.4
tests/test_wpa: tests/test_wpa.c wpa.c wpa.h wpa_drv.c wpa_drv.h tests/libmbedcrypto-host.a
	$(HOSTCC) -O1 -g -std=gnu89 -Wall -Wextra -Wdeclaration-after-statement -fsanitize=address,undefined \
	    -pthread -DWPA_DRV_TEST -I. -I$(MBEDTLS)/include -o $@ tests/test_wpa.c wpa.c wpa_drv.c log.c \
	    tests/libmbedcrypto-host.a

tests/test_mjpeg: tests/test_mjpeg.c mjpeg.c
	$(HOSTCC) -O2 -Wall -Wextra -I. -o $@ tests/test_mjpeg.c mjpeg.c

clean:
	rm -f $(OBJS) camd getmac tests/test_framebuf tests/test_config tests/test_ota tests/test_mjpeg tests/test_netcfg tests/test_wifi tests/test_wpa

.PHONY: all test clean
