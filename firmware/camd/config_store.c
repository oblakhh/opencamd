/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * config_store — JSON (de)serialization of the config and the A/B slot
 * store in the flash settings region (see config_store.h).
 */

#include "config_store.h"
#include "flash.h"
#include "crc32.h"
#include "log.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

/* This kernel has no MTD char device; write the block device (mtdblock erases
 * on write). The 4 KB erasesize puts the A/B slots in their own sectors, clear
 * of the bootloader / MAC below CFG_GUARD. */
#define CFG_DEV   "/dev/mtdblock0"
#define CFG_BASE  0xC000u
#define CFG_SIZE  0x4000u   /* 16 KB current-settings region */
#define CFG_SLOT  0x2000u   /* two 8 KB A/B slots */
#define CFG_GUARD 0x8000u   /* never write below here (protects MAC @ 0x6000) */
#define CFG_MAGIC "CAMD"
#define CFG_VER   1
#define HDR       20

/* ---- JSON ---------------------------------------------------------------- */

static int jint(const cJSON *o, const char *k, int def)
{
	const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
	return cJSON_IsNumber(i) ? i->valueint : def;
}
static void jstr(const cJSON *o, const char *k, char *dst, size_t n)
{
	const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
	if (cJSON_IsString(i) && i->valuestring) {
		strncpy(dst, i->valuestring, n - 1);
		dst[n - 1] = '\0';
	}
}

char *config_to_json(const config *c, int include_secrets)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *v = cJSON_AddObjectToObject(root, "video");
	cJSON *s = cJSON_AddObjectToObject(root, "system");
	cJSON *u = cJSON_AddObjectToObject(root, "upload");
	cJSON *net = cJSON_AddObjectToObject(root, "network");
	cJSON *nw = cJSON_AddObjectToObject(net, "wired");
	cJSON *wf = cJSON_AddObjectToObject(net, "wifi");
	char *out;

	cJSON_AddNumberToObject(v, "width", c->video.width);
	cJSON_AddNumberToObject(v, "height", c->video.height);
	cJSON_AddNumberToObject(v, "quality", c->video.quality);
	cJSON_AddNumberToObject(v, "target_fps", c->video.target_fps);

	cJSON_AddStringToObject(s, "hostname", c->system.hostname);
	cJSON_AddStringToObject(s, "ntp", c->system.ntp);
	cJSON_AddStringToObject(s, "syslog_remote", c->system.syslog_remote);
	cJSON_AddNumberToObject(s, "syslog_level", c->system.syslog_level);
	cJSON_AddBoolToObject(s, "telnet", c->system.telnet);
	if (include_secrets)
		cJSON_AddStringToObject(s, "pw_hash", c->system.pw_hash);

	cJSON_AddBoolToObject(u, "enabled", c->upload.enabled);
	cJSON_AddStringToObject(u, "url", c->upload.url);
	cJSON_AddNumberToObject(u, "interval", c->upload.interval);
	cJSON_AddStringToObject(u, "method", c->upload.method);
	cJSON_AddStringToObject(u, "field", c->upload.field);
	cJSON_AddStringToObject(u, "headers", c->upload.headers);

	cJSON_AddStringToObject(nw, "mode", c->network.wired.mode);
	cJSON_AddStringToObject(nw, "ip", c->network.wired.ip);
	cJSON_AddStringToObject(nw, "netmask", c->network.wired.netmask);
	cJSON_AddStringToObject(nw, "gateway", c->network.wired.gateway);
	cJSON_AddStringToObject(nw, "dns", c->network.wired.dns);
	cJSON_AddBoolToObject(wf, "enabled", c->network.wifi.enabled);
	cJSON_AddStringToObject(wf, "ssid", c->network.wifi.ssid);
	cJSON_AddBoolToObject(wf, "passphrase_set", c->network.wifi.passphrase[0] != '\0');
	if (include_secrets)
		cJSON_AddStringToObject(wf, "passphrase", c->network.wifi.passphrase);

	out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return out;
}

int config_from_json(config *c, const char *json, int allow_secrets)
{
	cJSON *root = cJSON_Parse(json);
	cJSON *v, *s, *u;
	if (!root)
		return -1;
	v = cJSON_GetObjectItemCaseSensitive(root, "video");
	if (cJSON_IsObject(v)) {
		c->video.width = jint(v, "width", c->video.width);
		c->video.height = jint(v, "height", c->video.height);
		c->video.quality = jint(v, "quality", c->video.quality);
		c->video.target_fps = jint(v, "target_fps", c->video.target_fps);
	}
	s = cJSON_GetObjectItemCaseSensitive(root, "system");
	if (cJSON_IsObject(s)) {
		jstr(s, "hostname", c->system.hostname, sizeof(c->system.hostname));
		jstr(s, "ntp", c->system.ntp, sizeof(c->system.ntp));
		jstr(s, "syslog_remote", c->system.syslog_remote, sizeof(c->system.syslog_remote));
		c->system.syslog_level = jint(s, "syslog_level", c->system.syslog_level);
		{
			const cJSON *t = cJSON_GetObjectItemCaseSensitive(s, "telnet");
			if (cJSON_IsBool(t)) c->system.telnet = cJSON_IsTrue(t);
		}
		if (allow_secrets)
			jstr(s, "pw_hash", c->system.pw_hash, sizeof(c->system.pw_hash));
	}
	u = cJSON_GetObjectItemCaseSensitive(root, "upload");
	if (cJSON_IsObject(u)) {
		const cJSON *e = cJSON_GetObjectItemCaseSensitive(u, "enabled");
		if (cJSON_IsBool(e)) c->upload.enabled = cJSON_IsTrue(e);
		jstr(u, "url", c->upload.url, sizeof(c->upload.url));
		c->upload.interval = jint(u, "interval", c->upload.interval);
		jstr(u, "method", c->upload.method, sizeof(c->upload.method));
		jstr(u, "field", c->upload.field, sizeof(c->upload.field));
		jstr(u, "headers", c->upload.headers, sizeof(c->upload.headers));
	}
	/* network is only taken from trusted input (flash); the API changes it via
	 * /api/network so it always goes through validation + safe apply. */
	if (allow_secrets) {
		cJSON *n = cJSON_GetObjectItemCaseSensitive(root, "network");
		cJSON *w = n ? cJSON_GetObjectItemCaseSensitive(n, "wired") : NULL;
		if (cJSON_IsObject(w)) {
			jstr(w, "mode", c->network.wired.mode, sizeof(c->network.wired.mode));
			jstr(w, "ip", c->network.wired.ip, sizeof(c->network.wired.ip));
			jstr(w, "netmask", c->network.wired.netmask, sizeof(c->network.wired.netmask));
			jstr(w, "gateway", c->network.wired.gateway, sizeof(c->network.wired.gateway));
			jstr(w, "dns", c->network.wired.dns, sizeof(c->network.wired.dns));
		}
		w = n ? cJSON_GetObjectItemCaseSensitive(n, "wifi") : NULL;
		if (cJSON_IsObject(w)) {
			const cJSON *e = cJSON_GetObjectItemCaseSensitive(w, "enabled");
			if (cJSON_IsBool(e)) c->network.wifi.enabled = cJSON_IsTrue(e);
			jstr(w, "ssid", c->network.wifi.ssid, sizeof(c->network.wifi.ssid));
			jstr(w, "passphrase", c->network.wifi.passphrase, sizeof(c->network.wifi.passphrase));
		}
	}
	cJSON_Delete(root);
	return 0;
}

/* ---- slot I/O ------------------------------------------------------------ */

/* Read & validate a slot. On success fills *seq and returns a malloc'd NUL-
 * terminated json string (caller free); returns NULL if the slot is invalid. */
static char *slot_read(flash_region *r, uint32_t slot_off, uint32_t *seq)
{
	unsigned char hdr[HDR];
	uint32_t ver_len, len, crc_stored, crc_calc, s;
	char *json;

	if (flash_read(r, slot_off, hdr, HDR) != 0)
		return NULL;
	if (memcmp(hdr, CFG_MAGIC, 4) != 0)
		return NULL;
	memcpy(&ver_len, hdr + 4, 4);               /* u16 ver | u16 pad */
	if ((ver_len & 0xFFFF) != CFG_VER)
		return NULL;
	memcpy(&s, hdr + 8, 4);
	memcpy(&len, hdr + 12, 4);
	memcpy(&crc_stored, hdr + 16, 4);
	if (len == 0 || len > CFG_SLOT - HDR)
		return NULL;
	json = malloc(len + 1);
	if (!json)
		return NULL;
	if (flash_read(r, slot_off + HDR, json, len) != 0) {
		free(json);
		return NULL;
	}
	json[len] = '\0';
	crc_calc = crc32(json, len);
	if (crc_calc != crc_stored) {
		free(json);
		return NULL;
	}
	*seq = s;
	return json;
}

static int slot_write(flash_region *r, uint32_t slot_off, uint32_t seq, const char *json)
{
	size_t len = strlen(json);
	unsigned char hdr[HDR];
	uint32_t ver_len = CFG_VER;         /* u16 ver | u16 pad(0) */
	uint32_t crc = crc32(json, len);

	if (HDR + len > CFG_SLOT) {
		LOGE("config: serialized config too large (%u)", (unsigned)(HDR + len));
		return -1;
	}
	memcpy(hdr, CFG_MAGIC, 4);
	memcpy(hdr + 4, &ver_len, 4);
	memcpy(hdr + 8, &seq, 4);
	{ uint32_t l = (uint32_t)len; memcpy(hdr + 12, &l, 4); }
	memcpy(hdr + 16, &crc, 4);

	if (flash_erase(r, slot_off, CFG_SLOT) != 0)
		return -1;
	if (flash_write(r, slot_off, hdr, HDR) != 0)
		return -1;
	if (flash_write(r, slot_off + HDR, json, (uint32_t)len) != 0)
		return -1;
	return 0;
}

/* ---- public API ---------------------------------------------------------- */

int config_store_load(config *c, const char *dev)
{
	flash_region r;
	char *ja, *jb;
	uint32_t sa = 0, sb = 0;
	int used_defaults = 1;

	config_defaults(c);
	if (flash_open(&r, dev ? dev : CFG_DEV, CFG_BASE, CFG_SIZE, 4096, CFG_GUARD) != 0)
		return 1; /* no store yet: run on defaults */

	ja = slot_read(&r, 0, &sa);
	jb = slot_read(&r, CFG_SLOT, &sb);

	if (ja && (!jb || sa >= sb)) {
		if (config_from_json(c, ja, 1) == 0) used_defaults = 0;
	} else if (jb) {
		if (config_from_json(c, jb, 1) == 0) used_defaults = 0;
	}
	free(ja); free(jb);
	flash_close(&r);
	if (used_defaults)
		LOGW("config: no valid stored config, using defaults");
	return used_defaults ? 1 : 0;
}

int config_store_save(const config *c, const char *dev)
{
	flash_region r;
	char *ja, *jb, *json;
	uint32_t sa = 0, sb = 0, next, target_off;
	int rc;

	if (flash_open(&r, dev ? dev : CFG_DEV, CFG_BASE, CFG_SIZE, 4096, CFG_GUARD) != 0)
		return -1;

	/* Pick the slot NOT holding the current highest seq, and seq = max+1. */
	ja = slot_read(&r, 0, &sa);
	jb = slot_read(&r, CFG_SLOT, &sb);
	if (!ja) sa = 0;
	if (!jb) sb = 0;
	free(ja); free(jb);
	if (sa >= sb) { target_off = CFG_SLOT; next = sa + 1; }
	else          { target_off = 0;        next = sb + 1; }
	if (next == 0) next = 1; /* avoid seq 0 (treated as empty) */

	json = config_to_json(c, 1);
	if (!json) { flash_close(&r); return -1; }
	rc = slot_write(&r, target_off, next, json);
	free(json);
	flash_close(&r);
	if (rc == 0)
		LOGI("config: saved (slot %s, seq %u)", target_off ? "B" : "A", next);
	return rc;
}

int config_store_erase(const char *dev)
{
	flash_region r;
	unsigned char zero[256];
	int rc = 0;
	if (flash_open(&r, dev ? dev : CFG_DEV, CFG_BASE, CFG_SIZE, 4096, CFG_GUARD) != 0)
		return -1;
	memset(zero, 0, sizeof(zero));
	/* The mtdblock device erases-on-write, so flash_erase() is a no-op here —
	 * clobber each slot's magic/CRC header with a write instead, so both slots
	 * read invalid and load falls back to defaults on next boot. */
	if (flash_write(&r, 0, zero, sizeof(zero)) != 0)
		rc = -1;
	if (flash_write(&r, CFG_SLOT, zero, sizeof(zero)) != 0)
		rc = -1;
	flash_close(&r);
	if (rc == 0)
		LOGW("config: erased (factory reset)");
	return rc;
}
