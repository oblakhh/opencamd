/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * config — runtime configuration: the in-memory struct and its compiled-in
 * defaults. Persistence (JSON in the 0xC000 flash region) is config_store.c.
 */
#ifndef CAMD_CONFIG_H
#define CAMD_CONFIG_H

typedef struct {
	int width;
	int height;
	int quality;      /* JPEG quality hint 1..100 (applied where supported) */
	int target_fps;   /* requested fps; module caps ~10 regardless */
} video_cfg;

typedef struct {
	char hostname[64];
	char ntp[128];
	char syslog_remote[128]; /* "host[:port]" or "" */
	int syslog_level;
	int telnet;
	char pw_hash[100];       /* "salthex:sha256hex"; "" = default password */
} system_cfg;

/* PrusaLink/OctoPrint-style snapshot uploader: push the current JPEG to an HTTP
 * endpoint every `interval` seconds. Plain HTTP only (no TLS). */
typedef struct {
	int enabled;
	char url[256];       /* http://host[:port]/path */
	int interval;        /* seconds between uploads (>=1) */
	char method[8];      /* "POST" (multipart) or "PUT" (raw body) */
	char field[32];      /* multipart form field name; "" = raw JPEG body */
	char headers[512];   /* extra request headers, one "Key: Value" per line
	                        (newline-separated); e.g. PrusaLink Fingerprint+Token */
} upload_cfg;

/* Wired network on br0 (see netcfg.c). Strings are validated dotted quads. */
typedef struct {
	char mode[8];        /* "dhcp" | "static" */
	char ip[16];
	char netmask[16];
	char gateway[16];    /* "" = no default route */
	char dns[40];        /* up to two servers, space/comma separated */
} wired_cfg;

typedef struct {
	int enabled;
	char ssid[33];
	char passphrase[64];  /* secret: flash only, never returned by the API */
} wifi_cfg;

typedef struct {
	wired_cfg wired;
	wifi_cfg wifi;
} network_cfg;

typedef struct {
	video_cfg video;
	system_cfg system;
	upload_cfg upload;
	network_cfg network;    /* wifi added in a later phase */
} config;

void config_defaults(config *c);

#endif
