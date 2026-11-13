/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * http — civetweb handlers: live stream, snapshot, status, static UI and
 * the authenticated /api control plane (config, network, Wi-Fi, OTA).
 */

#include "http.h"
#include "log.h"
#include "auth.h"
#include "services.h"
#include "config_store.h"
#include "ota.h"
#include "netcfg.h"
#include "wifi.h"
#include "gpio.h"
#include "net.h"
#include "cJSON.h"
#include "civetweb.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BOUNDARY "camdframe"

/* Each live MJPEG stream pins one civetweb worker for its whole duration (the
 * loop blocks in mg_write). Some clients — notably Safari — open stream
 * connections they never drain, so without a cap a few of them exhaust the
 * worker pool and starve the control plane (status/config/OTA), locking the box.
 * We cap concurrent streams well below num_threads so control always has workers. */
#define MAX_STREAMS 2

/* Serializes the control plane: config reads/writes and capture reconfigure,
 * which run on civetweb worker threads and share ctx->cfg / ctx->cap. */
static pthread_mutex_t g_ctl = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t g_stream_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_streams;   /* active MJPEG streams */

static int stream_acquire(void)
{
	int ok;
	pthread_mutex_lock(&g_stream_lock);
	ok = g_streams < MAX_STREAMS;
	if (ok)
		g_streams++;
	pthread_mutex_unlock(&g_stream_lock);
	return ok;
}

static void stream_release(void)
{
	pthread_mutex_lock(&g_stream_lock);
	if (g_streams > 0)
		g_streams--;
	pthread_mutex_unlock(&g_stream_lock);
}

/* ---- network info (active uplink) ---------------------------------------- */

struct netinfo { char iface[16]; char ip[16]; char mac[18]; const char *link; };

static int iface_ipv4(int s, const char *name, char out[16])
{
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
	if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0 || !(ifr.ifr_flags & IFF_UP))
		return -1;
	if (ioctl(s, SIOCGIFADDR, &ifr) < 0)
		return -1;
	{
		struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
		if (sin->sin_addr.s_addr == 0)
			return -1;
			inet_ntop(AF_INET, &sin->sin_addr, out, 16);
	}
	return 0;
}

static void iface_mac(int s, const char *name, char out[18])
{
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
	out[0] = '\0';
	if (ioctl(s, SIOCGIFHWADDR, &ifr) == 0) {
		unsigned char *m = (unsigned char *)ifr.ifr_hwaddr.sa_data;
		snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
		         m[0], m[1], m[2], m[3], m[4], m[5]);
	}
}

static void get_netinfo(struct netinfo *ni)
{
	/* Follow the active uplink (net.c); with none, wired first. While Wi-Fi
	 * is the uplink eth0 still carries the 192.168.2.3 rescue address, so
	 * "first interface with an IPv4" would be wrong. */
	static const char *wired_first[] = { "eth0", "wlan0", NULL };
	static const char *wifi_first[] = { "wlan0", "eth0", NULL };
	const char **cands = strcmp(net_uplink(), "wifi") == 0 ? wifi_first : wired_first;
	int s, i;
	memset(ni, 0, sizeof(*ni));
	ni->link = "none";
	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		return;
	for (i = 0; cands[i]; i++) {
		if (iface_ipv4(s, cands[i], ni->ip) == 0) {
			snprintf(ni->iface, sizeof(ni->iface), "%s", cands[i]);
			iface_mac(s, cands[i], ni->mac);
			ni->link = (strncmp(cands[i], "wlan", 4) == 0) ? "wifi" : "wired";
			break;
		}
	}
	close(s);
}

/* ---- auth helpers -------------------------------------------------------- */

static int req_is_admin(struct mg_connection *c)
{
	const char *cookie = mg_get_header(c, "Cookie");
	char tok[64];
	if (cookie && mg_get_cookie(cookie, "camd_session", tok, sizeof(tok)) > 0 &&
	    auth_valid_session(tok))
		return 1;
	return auth_check_basic(mg_get_header(c, "Authorization"));
}

/* CSRF defense via Fetch Metadata. Browsers set Sec-Fetch-Site unforgeably;
 * a foreign form/page POSTing to us is tagged "cross-site" (or "same-site").
 * Non-browser clients (curl, scripts, integrations) omit the header entirely,
 * so we only reject when it is present AND not same-origin. Covers the Basic-
 * auth vector that SameSite=Strict on the session cookie does not. */
static int req_csrf_ok(struct mg_connection *c)
{
	const char *s = mg_get_header(c, "Sec-Fetch-Site");
	if (!s)
		return 1;
	return strcmp(s, "same-origin") == 0 || strcmp(s, "none") == 0;
}

static int send_403_csrf(struct mg_connection *c)
{
	mg_printf(c, "HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\n"
	             "Connection: close\r\n\r\n{\"ok\":false,\"error\":\"cross-site request blocked\"}");
	return 403;
}

static int send_401(struct mg_connection *c)
{
	mg_printf(c, "HTTP/1.1 401 Unauthorized\r\n"
	             "WWW-Authenticate: Basic realm=\"camd\"\r\n"
	             "Content-Type: application/json\r\nConnection: close\r\n\r\n"
	             "{\"ok\":false}");
	return 401;
}

/* ---- handlers ------------------------------------------------------------ */

static int h_snapshot(struct mg_connection *c, void *cbdata)
{
	http_ctx *ctx = cbdata;
	frame_ref ref;
	int rc = framepool_acquire(ctx->pool, 0, 3000, &ref);
	if (rc != 0) {
		mg_printf(c, "HTTP/1.1 503 Service Unavailable\r\n"
		             "Content-Type: text/plain\r\nConnection: close\r\n\r\n"
		             "no frame yet\n");
		return 503;
	}
	mg_printf(c, "HTTP/1.1 200 OK\r\n"
	             "Content-Type: image/jpeg\r\n"
	             "Content-Length: %u\r\n"
	             "Cache-Control: no-store\r\n"
	             "Connection: close\r\n\r\n", (unsigned)ref.len);
	mg_write(c, ref.data, ref.len);
	framepool_release(ctx->pool, &ref);
	return 200;
}

static int h_stream(struct mg_connection *c, void *cbdata)
{
	http_ctx *ctx = cbdata;
	uint32_t last = 0;
	int misses = 0;

	if (!stream_acquire()) {
		mg_printf(c, "HTTP/1.1 503 Service Unavailable\r\n"
		             "Content-Type: text/plain\r\nRetry-After: 5\r\n"
		             "Connection: close\r\n\r\ntoo many viewers\n");
		return 503;
	}

	{
		const struct mg_request_info *ri = mg_get_request_info(c);
		int raw = ri && ri->query_string && strstr(ri->query_string, "raw") != NULL;
		/* Safari refuses to stream a multipart/x-mixed-replace body via fetch().
		 * The SPA requests ?raw=1 to get the identical framing under a plain
		 * content-type (which every browser streams); native consumers (HA, VLC,
		 * a direct <img>) use the default multipart type. */
		if (raw)
			mg_printf(c, "HTTP/1.1 200 OK\r\n"
			             "Content-Type: application/octet-stream\r\n"
			             "X-Stream-Boundary: " BOUNDARY "\r\n"
			             "Cache-Control: no-store\r\nConnection: close\r\n\r\n");
		else
			mg_printf(c, "HTTP/1.1 200 OK\r\n"
			             "Content-Type: multipart/x-mixed-replace; boundary=" BOUNDARY "\r\n"
			             "Cache-Control: no-store\r\nConnection: close\r\n\r\n");
	}

	for (;;) {
		frame_ref ref;
		char hdr[128];
		int n, rc = framepool_acquire(ctx->pool, last, 5000, &ref);
		if (rc != 0) {
			if (++misses > 3)
				break; /* camera gone for ~15s; end the stream */
			continue;
		}
		misses = 0;
		n = snprintf(hdr, sizeof(hdr),
		             "--" BOUNDARY "\r\nContent-Type: image/jpeg\r\n"
		             "Content-Length: %u\r\n\r\n", (unsigned)ref.len);
		if (mg_write(c, hdr, (size_t)n) <= 0 ||
		    mg_write(c, ref.data, ref.len) <= 0 ||
		    mg_write(c, "\r\n", 2) <= 0) {
			framepool_release(ctx->pool, &ref);
			break; /* client disconnected */
		}
		last = ref.seq;
		framepool_release(ctx->pool, &ref);
	}
	stream_release();
	return 200;
}

static int h_status(struct mg_connection *c, void *cbdata)
{
	http_ctx *ctx = cbdata;
	struct netinfo ni;
	char tbuf[32];
	time_t now = time(NULL);
	struct tm tm_utc;
	long up = 0;
	FILE *f;

	cJSON *root, *v, *net;
	char *out;
	int admin = req_is_admin(c);
	get_netinfo(&ni);
	gmtime_r(&now, &tm_utc);
	strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
	if ((f = fopen("/proc/uptime", "r"))) { double u; if (fscanf(f, "%lf", &u) == 1) up = (long)u; fclose(f); }

	root = cJSON_CreateObject();
	pthread_mutex_lock(&g_ctl); /* cfg/cap are shared with the config/reconfigure path */
	cJSON_AddStringToObject(root, "name", ctx->cfg->system.hostname);
	cJSON_AddStringToObject(root, "model", "IC-3115W");
	cJSON_AddStringToObject(root, "version", ctx->version);
	cJSON_AddNumberToObject(root, "uptime", up);
	cJSON_AddStringToObject(root, "time", tbuf);
	v = cJSON_AddObjectToObject(root, "video");
	cJSON_AddNumberToObject(v, "width", ctx->cap->width);
	cJSON_AddNumberToObject(v, "height", ctx->cap->height);
	cJSON_AddNumberToObject(v, "fps", ctx->cap->fps_measured);
	cJSON_AddNumberToObject(v, "quality", ctx->cfg->video.quality);
	cJSON_AddBoolToObject(v, "alive", ctx->cap->alive);
	pthread_mutex_unlock(&g_ctl);
	net = cJSON_AddObjectToObject(root, "net");
	cJSON_AddStringToObject(net, "iface", ni.iface);
	cJSON_AddStringToObject(net, "ip", ni.ip);
	cJSON_AddStringToObject(net, "mac", ni.mac);
	cJSON_AddStringToObject(net, "link", ni.link);
	cJSON_AddStringToObject(net, "mode", ctx->cfg->network.wired.mode);
	cJSON_AddStringToObject(net, "uplink", net_uplink());
	cJSON_AddBoolToObject(net, "wired_link", eth_link_up());
	{
		cJSON *ps = cJSON_AddArrayToObject(net, "psr");   /* raw PSRP0..4 (probe) */
		int p; unsigned v;
		for (p = 0; p < 5; p++)
			cJSON_AddItemToArray(ps, cJSON_CreateNumber(eth_port_status(p, &v) == 0 ? (double)v : -1));
	}
	cJSON_AddNumberToObject(net, "revert_in", netcfg_pending_left());
	{
		wifi_status ws;
		cJSON *wj = cJSON_AddObjectToObject(net, "wifi");
		wifi_get_status(&ctx->cfg->network.wifi, &ws);
		if (strcmp(net_uplink(), "wired") == 0 && strcmp(ws.state, "off") != 0) {
			/* cable is the uplink: wlan0 is only up for a test or scan */
			ws.state = "standby";
			ws.bssid[0] = ws.ip[0] = '\0';
			ws.rssi = -1;
		}
		cJSON_AddStringToObject(wj, "state", ws.state);
		cJSON_AddStringToObject(wj, "ssid", ws.ssid);
		cJSON_AddStringToObject(wj, "bssid", ws.bssid);
		cJSON_AddStringToObject(wj, "ip", ws.ip);
		cJSON_AddNumberToObject(wj, "rssi", ws.rssi);
	}
	{
		cJSON *up = cJSON_AddObjectToObject(root, "upload");
		cJSON_AddBoolToObject(up, "enabled", ctx->cfg->upload.enabled);
		cJSON_AddBoolToObject(up, "ok", ctx->up->ok);
		cJSON_AddNumberToObject(up, "last_code", ctx->up->last_code);
		cJSON_AddNumberToObject(up, "count", (double)ctx->up->count);
	}
	cJSON_AddBoolToObject(root, "admin", admin);
	out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
	             "Cache-Control: no-store\r\nConnection: close\r\n\r\n%s", out ? out : "{}");
	free(out);
	return 200;
}

static int read_body(struct mg_connection *c, char *buf, int max)
{
	int n = mg_read(c, buf, max - 1);
	if (n < 0) n = 0;
	buf[n] = '\0';
	return n;
}

static int h_login(struct mg_connection *c, void *cbdata)
{
	char body[256], tok[64];
	cJSON *j;
	const cJSON *pw;
	(void)cbdata;
	read_body(c, body, sizeof(body));
	j = cJSON_Parse(body);
	pw = j ? cJSON_GetObjectItemCaseSensitive(j, "password") : NULL;
	if (cJSON_IsString(pw) && auth_check_password(pw->valuestring) && auth_login(tok, sizeof(tok))) {
		cJSON_Delete(j);
		mg_printf(c, "HTTP/1.1 200 OK\r\n"
		             "Set-Cookie: camd_session=%s; Path=/; HttpOnly; SameSite=Strict\r\n"
		             "Content-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":true}", tok);
		return 200;
	}
	cJSON_Delete(j);
	mg_printf(c, "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\n"
	             "Connection: close\r\n\r\n{\"ok\":false}");
	return 401;
}

static int h_logout(struct mg_connection *c, void *cbdata)
{
	const char *cookie = mg_get_header(c, "Cookie");
	char tok[64];
	(void)cbdata;
	if (cookie && mg_get_cookie(cookie, "camd_session", tok, sizeof(tok)) > 0)
		auth_logout(tok);
	mg_printf(c, "HTTP/1.1 200 OK\r\nSet-Cookie: camd_session=; Path=/; Max-Age=0\r\n"
	             "Content-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":true}");
	return 200;
}

static int video_mode_ok(int w, int h)
{
	static const int modes[9][2] = {
		{1280, 1024}, {1280, 960}, {1280, 800}, {1280, 720}, {640, 480},
		{352, 288}, {320, 240}, {176, 144}, {160, 120}
	};
	int i;
	for (i = 0; i < 9; i++)
		if (modes[i][0] == w && modes[i][1] == h)
			return 1;
	return 0;
}

static int h_config(struct mg_connection *c, void *cbdata)
{
	http_ctx *ctx = cbdata;
	const struct mg_request_info *ri = mg_get_request_info(c);
	char *jj;

	if (!req_is_admin(c))
		return send_401(c);

	if (strcmp(ri->request_method, "GET") == 0) {
		pthread_mutex_lock(&g_ctl);
		jj = config_to_json(ctx->cfg, 0);
		pthread_mutex_unlock(&g_ctl);
		mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
		             "Cache-Control: no-store\r\nConnection: close\r\n\r\n%s", jj ? jj : "{}");
		free(jj);
		return 200;
	}
	/* PUT/POST: validate on a copy, then commit + apply + persist under the lock. */
	if (!req_csrf_ok(c))
		return send_403_csrf(c);
	{
		char body[8192];
		config nc;
		int ow, oh, of;
		read_body(c, body, sizeof(body));

		pthread_mutex_lock(&g_ctl);
		nc = *ctx->cfg;
		if (config_from_json(&nc, body, 0) != 0) {
			pthread_mutex_unlock(&g_ctl);
			mg_printf(c, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
			             "Connection: close\r\n\r\n{\"ok\":false,\"error\":\"bad json\"}");
			return 400;
		}
		if (!video_mode_ok(nc.video.width, nc.video.height) ||
		    nc.video.quality < 1 || nc.video.quality > 100 ||
		    nc.video.target_fps < 1 || nc.video.target_fps > 30) {
			pthread_mutex_unlock(&g_ctl);
			mg_printf(c, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
			             "Connection: close\r\n\r\n{\"ok\":false,\"error\":\"invalid video settings\"}");
			return 400;
		}
		if (!services_syslog_target_ok(nc.system.syslog_remote) ||
		    nc.system.syslog_level < 0 || nc.system.syslog_level > 7) {
			pthread_mutex_unlock(&g_ctl);
			mg_printf(c, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
			             "Connection: close\r\n\r\n{\"ok\":false,\"error\":\"invalid syslog settings\"}");
			return 400;
		}
		if (nc.upload.enabled &&
		    ((strncmp(nc.upload.url, "http://", 7) != 0 &&
		      strncmp(nc.upload.url, "https://", 8) != 0) ||
		     nc.upload.interval < 1 || nc.upload.interval > 3600 ||
		     (strcmp(nc.upload.method, "POST") != 0 && strcmp(nc.upload.method, "PUT") != 0))) {
			pthread_mutex_unlock(&g_ctl);
			mg_printf(c, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
			             "Connection: close\r\n\r\n{\"ok\":false,\"error\":\"invalid upload settings\"}");
			return 400;
		}
		ow = ctx->cfg->video.width; oh = ctx->cfg->video.height; of = ctx->cfg->video.target_fps;
		*ctx->cfg = nc; /* commit only the validated config */
		if (nc.video.width != ow || nc.video.height != oh || nc.video.target_fps != of)
			capture_reconfigure(ctx->cap, nc.video.width, nc.video.height, nc.video.target_fps);
		config_store_save(ctx->cfg, ctx->cfgdev);
		services_apply_telnet(nc.system.telnet);
		services_apply_syslog(nc.system.syslog_remote, nc.system.syslog_level);
		jj = config_to_json(ctx->cfg, 0);
		pthread_mutex_unlock(&g_ctl);
		mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
		             "Connection: close\r\n\r\n%s", jj ? jj : "{}");
		free(jj);
		return 200;
	}
}

/* Read-only device introspection (admin): dump a whitelist of /proc + version
 * files as text. Handy for remote debugging over HTTP. */
static int h_debug(struct mg_connection *c, void *cbdata)
{
	static const char *files[] = {
		"/proc/mtd", "/proc/meminfo", "/proc/mounts", "/proc/uptime",
		"/proc/loadavg", "/etc/version", NULL
	};
	int i;
	(void)cbdata;
	if (!req_is_admin(c))
		return send_401(c);
	mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
	             "Cache-Control: no-store\r\nConnection: close\r\n\r\n");
	mg_printf(c, "camd %s  streams=%d/%d\n\n", "debug", g_streams, MAX_STREAMS);
	for (i = 0; files[i]; i++) {
		FILE *f = fopen(files[i], "r");
		mg_printf(c, "===== %s =====\n", files[i]);
		if (f) {
			char buf[512];
			size_t n;
			while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
				mg_write(c, buf, n);
			fclose(f);
		} else {
			mg_printf(c, "(cannot open)\n");
		}
		mg_printf(c, "\n");
	}
	return 200;
}

/* ---- OTA + reboot -------------------------------------------------------- */

/* Serializes firmware updates / reboots: at most one, and once committed it is
 * never unlocked (the box is going down). */
static pthread_mutex_t g_ota = PTHREAD_MUTEX_INITIALIZER;

static int h_ota(struct mg_connection *c, void *cbdata)
{
	const struct mg_request_info *ri = mg_get_request_info(c);
	long long clen = ri ? ri->content_length : -1;
	unsigned char *buf;
	size_t cap, len = 0;
	const char *err = NULL;
	int n, rc, armed;
	(void)cbdata;

	if (!req_is_admin(c))
		return send_401(c);
	if (!ri || (strcmp(ri->request_method, "POST") != 0 &&
	            strcmp(ri->request_method, "PUT") != 0)) {
		mg_printf(c, "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n\r\n");
		return 405;
	}
	if (!req_csrf_ok(c))
		return send_403_csrf(c);
	if (clen >= 0 && clen < 642) {   /* smaller than any squashfs + checksum */
		mg_printf(c, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
		             "Connection: close\r\n\r\n{\"ok\":false,\"error\":\"image too small\"}");
		return 400;
	}
	if (clen > (long long)OTA_MAX) {
		mg_printf(c, "HTTP/1.1 413 Payload Too Large\r\nContent-Type: application/json\r\n"
		             "Connection: close\r\n\r\n{\"ok\":false,\"error\":\"image too large\"}");
		return 413;
	}
	if (pthread_mutex_trylock(&g_ota) != 0) {
		mg_printf(c, "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\nConnection: close"
		             "\r\n\r\n{\"ok\":false,\"error\":\"an update is already in progress\"}");
		return 409;
	}
	ota_in_progress = 1;
	cap = (clen > 0 && (size_t)clen < OTA_MAX) ? (size_t)clen : OTA_MAX;
	buf = malloc(cap);
	if (!buf) {
		ota_in_progress = 0;
		pthread_mutex_unlock(&g_ota);
		mg_printf(c, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\n"
		             "Connection: close\r\n\r\n{\"ok\":false,\"error\":\"out of memory\"}");
		return 500;
	}
	while (len < cap && (n = mg_read(c, buf + len, cap - len)) > 0)
		len += (size_t)n;
	if (clen > 0 && (long long)len < clen) {   /* connection stalled or dropped */
		free(buf);
		ota_in_progress = 0;
		pthread_mutex_unlock(&g_ota);
		LOGW("ota: upload incomplete, %lu of %lld bytes", (unsigned long)len, clen);
		mg_printf(c, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
		             "Connection: close\r\n\r\n{\"ok\":false,\"error\":\"upload incomplete: received %lu of %lld bytes\"}",
		          (unsigned long)len, clen);
		return 400;
	}
	/* Pre-map the reset register page BEFORE flashing, so the post-flash reset
	 * path touches no (now-rewritten) filesystem. */
	armed = (ota_arm_reset() == 0);
	rc = ota_apply_rootfs(buf, len, &err);
	free(buf);
	if (rc != 0) {
		ota_in_progress = 0;
		pthread_mutex_unlock(&g_ota);
		LOGW("ota: apply failed: %s", err ? err : "?");
		mg_printf(c, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\n"
		             "Connection: close\r\n\r\n{\"ok\":false,\"error\":\"%s\"}", err ? err : "ota failed");
		return 500;
	}
	/* The new rootfs is on flash but the old one is still mounted. Tell the
	 * client, then reset immediately (g_ota stays held — we are going down). */
	mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
	             "{\"ok\":true,\"bytes\":%lu,\"rebooting\":%s}",
	          (unsigned long)len, armed ? "true" : "false");
	LOGI("ota: flashed %lu bytes; %s", (unsigned long)len,
	     armed ? "resetting" : "arm failed, power-cycle to apply");
	if (armed) {
		usleep(400 * 1000);   /* let the response flush */
		ota_fire_reset();     /* does not return */
	}
	return 200;               /* could not arm reset: operator power-cycles */
}

static void *reboot_thread(void *a)
{
	(void)a;
	sleep(1);            /* let the HTTP response flush before the SoC resets */
	ota_fire_reset();    /* does not return */
	return NULL;
}

static int h_reboot(struct mg_connection *c, void *cbdata)
{
	const struct mg_request_info *ri = mg_get_request_info(c);
	pthread_t t;
	(void)cbdata;
	if (!req_is_admin(c))
		return send_401(c);
	if (!ri || strcmp(ri->request_method, "POST") != 0) {
		mg_printf(c, "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n\r\n");
		return 405;
	}
	if (!req_csrf_ok(c))
		return send_403_csrf(c);
	if (pthread_mutex_trylock(&g_ota) != 0) {   /* an update/reboot is already committed */
		mg_printf(c, "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\nConnection: close"
		             "\r\n\r\n{\"ok\":false,\"error\":\"an update is in progress\"}");
		return 409;
	}
	if (ota_arm_reset() != 0 || pthread_create(&t, NULL, reboot_thread, NULL) != 0) {
		pthread_mutex_unlock(&g_ota);
		mg_printf(c, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\n"
		             "Connection: close\r\n\r\n{\"ok\":false,\"error\":\"cannot arm reset\"}");
		return 500;
	}
	pthread_detach(t);
	mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
	             "{\"ok\":true,\"msg\":\"rebooting\"}");
	return 200;
}

static int h_factory_reset(struct mg_connection *c, void *cbdata)
{
	const struct mg_request_info *ri = mg_get_request_info(c);
	pthread_t t;
	(void)cbdata;
	if (!req_is_admin(c))
		return send_401(c);
	if (!ri || strcmp(ri->request_method, "POST") != 0) {
		mg_printf(c, "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n\r\n");
		return 405;
	}
	if (!req_csrf_ok(c))
		return send_403_csrf(c);
	if (pthread_mutex_trylock(&g_ota) != 0) {
		mg_printf(c, "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\nConnection: close"
		             "\r\n\r\n{\"ok\":false,\"error\":\"an update is in progress\"}");
		return 409;
	}
	if (config_store_erase(NULL) != 0) {
		pthread_mutex_unlock(&g_ota);
		mg_printf(c, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\n"
		             "Connection: close\r\n\r\n{\"ok\":false,\"error\":\"could not erase settings\"}");
		return 500;
	}
	LOGW("factory reset: settings erased, rebooting to defaults");
	if (ota_arm_reset() != 0 || pthread_create(&t, NULL, reboot_thread, NULL) != 0) {
		pthread_mutex_unlock(&g_ota);
		mg_printf(c, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\n"
		             "Connection: close\r\n\r\n{\"ok\":false,\"error\":\"cannot arm reset\"}");
		return 500;
	}
	pthread_detach(t);
	mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
	             "{\"ok\":true,\"msg\":\"factory reset\"}");
	return 200;
}

static int h_password(struct mg_connection *c, void *cbdata)
{
	http_ctx *ctx = cbdata;
	const struct mg_request_info *ri = mg_get_request_info(c);
	char body[256];
	cJSON *j;
	const cJSON *pw;
	size_t plen;

	if (!req_is_admin(c))
		return send_401(c);
	if (!ri || strcmp(ri->request_method, "POST") != 0) {
		mg_printf(c, "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n\r\n");
		return 405;
	}
	if (!req_csrf_ok(c))
		return send_403_csrf(c);
	read_body(c, body, sizeof(body));
	j = cJSON_Parse(body);
	pw = j ? cJSON_GetObjectItemCaseSensitive(j, "password") : NULL;
	plen = (cJSON_IsString(pw) && pw->valuestring) ? strlen(pw->valuestring) : 0;
	if (plen < 1 || plen > 63) {
		cJSON_Delete(j);
		mg_printf(c, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
		             "Connection: close\r\n\r\n{\"ok\":false,\"error\":\"password must be 1-63 characters\"}");
		return 400;
	}
	pthread_mutex_lock(&g_ctl);
	auth_set_password(pw->valuestring);
	auth_get_password_hash(ctx->cfg->system.pw_hash, sizeof(ctx->cfg->system.pw_hash));
	config_store_save(ctx->cfg, ctx->cfgdev);
	auth_write_system_passwd("/var/passwd");
	pthread_mutex_unlock(&g_ctl);
	cJSON_Delete(j);
	LOGI("admin password changed");
	mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
	             "{\"ok\":true}");
	return 200;
}

/* Copy a string member of a JSON object into dst (if present). */
static void jset(const cJSON *o, const char *k, char *dst, size_t n)
{
	const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
	if (cJSON_IsString(i) && i->valuestring)
		snprintf(dst, n, "%s", i->valuestring);
}

/* POST /api/network {"wired":{mode,ip,netmask,gateway,dns}} -> apply live, revert
 * in 90 s unless POST /api/network/confirm arrives. */
static int h_network(struct mg_connection *c, void *cbdata)
{
	http_ctx *ctx = cbdata;
	const struct mg_request_info *ri = mg_get_request_info(c);
	const char *uri = ri && ri->local_uri ? ri->local_uri : "";
	char body[1024];
	cJSON *j, *wd;
	wired_cfg w, committed;
	const char *err = NULL;

	if (!req_is_admin(c))
		return send_401(c);
	if (!ri || (strcmp(ri->request_method, "POST") != 0 &&
	            strcmp(ri->request_method, "PUT") != 0)) {
		mg_printf(c, "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n\r\n");
		return 405;
	}
	if (!req_csrf_ok(c))
		return send_403_csrf(c);

	if (strstr(uri, "/confirm")) {
		if (!netcfg_confirm(&w)) {
			mg_printf(c, "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\n"
			             "Connection: close\r\n\r\n{\"ok\":false,\"error\":\"nothing to confirm\"}");
			return 409;
		}
		pthread_mutex_lock(&g_ctl);
		ctx->cfg->network.wired = w;
		config_store_save(ctx->cfg, ctx->cfgdev);
		pthread_mutex_unlock(&g_ctl);
		net_set_wired(&w);
		LOGI("netcfg: change confirmed and saved");
		mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
		             "Connection: close\r\n\r\n{\"ok\":true,\"confirmed\":true}");
		return 200;
	}

	read_body(c, body, sizeof(body));
	j = cJSON_Parse(body);
	wd = j ? cJSON_GetObjectItemCaseSensitive(j, "wired") : NULL;
	{
		/* {"wifi":{enabled, ssid, passphrase}}: no stranding risk while the
		 * wired link is up, so apply + persist immediately. An omitted/empty
		 * passphrase keeps the stored one. */
		cJSON *wf = j ? cJSON_GetObjectItemCaseSensitive(j, "wifi") : NULL;
		if (cJSON_IsObject(wf) && !wd) {
			wifi_cfg nw;
			const cJSON *en = cJSON_GetObjectItemCaseSensitive(wf, "enabled");
			char pass[64] = "";
			pthread_mutex_lock(&g_ctl);
			nw = ctx->cfg->network.wifi;
			pthread_mutex_unlock(&g_ctl);
			if (cJSON_IsBool(en)) nw.enabled = cJSON_IsTrue(en);
			jset(wf, "ssid", nw.ssid, sizeof(nw.ssid));
			jset(wf, "passphrase", pass, sizeof(pass));
			if (pass[0]) memcpy(nw.passphrase, pass, sizeof(pass));
			memset(pass, 0, sizeof(pass));
			cJSON_Delete(j);
			memset(body, 0, sizeof(body));
			if (wifi_validate(&nw, &err) != 0) {
				mg_printf(c, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
				             "Connection: close\r\n\r\n{\"ok\":false,\"error\":\"%s\"}", err);
				return 400;
			}
			pthread_mutex_lock(&g_ctl);
			ctx->cfg->network.wifi = nw;
			config_store_save(ctx->cfg, ctx->cfgdev);
			pthread_mutex_unlock(&g_ctl);
			net_set_wifi(&nw);   /* net.c decides whether Wi-Fi comes up */
			memset(&nw, 0, sizeof(nw));
			mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
			             "Connection: close\r\n\r\n{\"ok\":true,\"wifi\":\"applying\"}");
			return 200;
		}
	}
	pthread_mutex_lock(&g_ctl);
	committed = ctx->cfg->network.wired;
	pthread_mutex_unlock(&g_ctl);
	w = committed;
	if (cJSON_IsObject(wd) && strcmp(net_uplink(), "wired") != 0) {
		cJSON_Delete(j);
		mg_printf(c, "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
		             "{\"ok\":false,\"error\":\"wired settings can only be changed while the cable is the uplink\"}");
		return 409;
	}
	if (!cJSON_IsObject(wd)) {
		err = "expected {\"wired\":{...}}";
	} else {
		jset(wd, "mode", w.mode, sizeof(w.mode));
		jset(wd, "ip", w.ip, sizeof(w.ip));
		jset(wd, "netmask", w.netmask, sizeof(w.netmask));
		jset(wd, "gateway", w.gateway, sizeof(w.gateway));
		jset(wd, "dns", w.dns, sizeof(w.dns));
		netcfg_validate(&w, &err);
	}
	cJSON_Delete(j);
	if (err) {
		mg_printf(c, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
		             "Connection: close\r\n\r\n{\"ok\":false,\"error\":\"%s\"}", err);
		return 400;
	}
	netcfg_try(&w, &committed, ctx->cfg->system.hostname, 90);
	mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
	             "{\"ok\":true,\"mode\":\"%s\",\"ip\":\"%s\",\"revert_in\":90}",
	          w.mode, strcmp(w.mode, "static") == 0 ? w.ip : "");
	return 200;
}

/* POST /api/wifi/scan -> [{ssid,bssid,channel}] (takes a few seconds). */
static int h_wifi_scan(struct mg_connection *c, void *cbdata)
{
	const struct mg_request_info *ri = mg_get_request_info(c);
	cJSON *arr;
	char *out;
	(void)cbdata;
	if (!req_is_admin(c))
		return send_401(c);
	if (!ri || strcmp(ri->request_method, "POST") != 0) {
		mg_printf(c, "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n\r\n");
		return 405;
	}
	if (!req_csrf_ok(c))
		return send_403_csrf(c);
	net_wlan_lock();      /* not while net.c switches uplinks or a test runs */
	arr = wifi_scan();
	net_wlan_unlock();
	out = cJSON_PrintUnformatted(arr);
	cJSON_Delete(arr);
	mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n"
	             "Connection: close\r\n\r\n%s", out ? out : "[]");
	free(out);
	return 200;
}

/* POST /api/wifi/test {"ssid":"...","passphrase":"..."?} -> {"result":
 * ok|wrong_passphrase|not_found|no_dhcp|busy|error, "ip"} (synchronous, <= ~25 s).
 * Only while the cable is the uplink; joins with a locally administered MAC and
 * takes wlan0 down again. An omitted passphrase means the stored one. */
static int h_wifi_test(struct mg_connection *c, void *cbdata)
{
	http_ctx *ctx = cbdata;
	const struct mg_request_info *ri = mg_get_request_info(c);
	char body[1024], pass[64] = "", ip[16] = "";
	cJSON *j;
	wifi_cfg t;
	const char *err = NULL;
	int r, b, ms = 0, aborted = 0, cool = 0;

	if (!req_is_admin(c))
		return send_401(c);
	if (!ri || strcmp(ri->request_method, "POST") != 0) {
		mg_printf(c, "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n\r\n");
		return 405;
	}
	if (!req_csrf_ok(c))
		return send_403_csrf(c);

	read_body(c, body, sizeof(body));
	j = cJSON_Parse(body);
	memset(body, 0, sizeof(body));
	pthread_mutex_lock(&g_ctl);
	t = ctx->cfg->network.wifi;
	pthread_mutex_unlock(&g_ctl);
	t.enabled = 1;
	t.ssid[0] = '\0';
	if (j) {
		jset(j, "ssid", t.ssid, sizeof(t.ssid));
		jset(j, "passphrase", pass, sizeof(pass));
		{
			cJSON *pi = cJSON_GetObjectItemCaseSensitive(j, "passphrase");
			if (cJSON_IsString(pi) && pi->valuestring)
				memset(pi->valuestring, 0, strlen(pi->valuestring));
		}
		cJSON_Delete(j);
	}
	if (pass[0])
		memcpy(t.passphrase, pass, sizeof(pass));
	memset(pass, 0, sizeof(pass));
	if (!j)
		err = "expected {\"ssid\":\"...\",\"passphrase\":\"...\"}";
	else if (!t.passphrase[0])
		err = "no passphrase given and none stored";
	else
		wifi_validate(&t, &err);
	if (err) {
		memset(&t, 0, sizeof(t));
		mg_printf(c, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
		             "Connection: close\r\n\r\n{\"ok\":false,\"error\":\"%s\"}", err);
		return 400;
	}
	b = net_wlan_test_begin();
	if (b == 0 && (cool = wifi_test_cooldown()) > 0) {
		net_wlan_test_end();
		b = -3;
	}
	if (b == -1) {
		memset(&t, 0, sizeof(t));
		mg_printf(c, "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
		             "{\"ok\":false,\"error\":\"a Wi-Fi test is only possible while the cable is the uplink\"}");
		return 409;
	}
	if (b == -2 || b == -3) {
		r = WT_BUSY;
	} else {
		r = wifi_test(t.ssid, t.passphrase, 1, net_wlan_aborted, ip, &ms);
		aborted = r == WT_ERROR && net_wlan_aborted();
		net_wlan_test_end();
	}
	{
		cJSON *o = cJSON_CreateObject();
		char *out;
		cJSON_AddBoolToObject(o, "ok", r == WT_OK);
		cJSON_AddStringToObject(o, "result", wifi_test_name(r));
		cJSON_AddStringToObject(o, "ssid", t.ssid);
		if (r == WT_OK)
			cJSON_AddStringToObject(o, "ip", ip);
		if (r != WT_BUSY)
			cJSON_AddNumberToObject(o, "join_ms", ms);
		if (b == -3)   /* the AP is still in its cool-down after a rejected test */
			cJSON_AddNumberToObject(o, "retry_in", cool);
		if (aborted)
			cJSON_AddStringToObject(o, "error", "aborted: the cable went away during the test");
		memset(&t, 0, sizeof(t));
		out = cJSON_PrintUnformatted(o);
		cJSON_Delete(o);
		mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n"
		             "Connection: close\r\n\r\n%s", out ? out : "{}");
		free(out);
	}
	return 200;
}

/* ---- server lifecycle ---------------------------------------------------- */

struct mg_context *http_start(http_ctx *ctx, int port, const char *docroot)
{
	char ports[16];
	struct mg_callbacks cb;
	struct mg_context *mg;
	const char *opts[11];
	int n = 0;

	snprintf(ports, sizeof(ports), "%d", port);
	opts[n++] = "listening_ports";   opts[n++] = ports;
	opts[n++] = "num_threads";       opts[n++] = "6";  /* >= MAX_STREAMS + control reserve */
	opts[n++] = "enable_directory_listing"; opts[n++] = "no";
	if (docroot && docroot[0]) { opts[n++] = "document_root"; opts[n++] = docroot; }
	opts[n] = NULL;

	memset(&cb, 0, sizeof(cb));
	mg = mg_start(&cb, NULL, opts);
	if (!mg) {
		LOGE("http: mg_start failed on port %d", port);
		return NULL;
	}
	mg_set_request_handler(mg, "/snapshot.jpg", h_snapshot, ctx);
	mg_set_request_handler(mg, "/snapshot.cgi", h_snapshot, ctx); /* legacy alias */
	mg_set_request_handler(mg, "/stream.mjpg", h_stream, ctx);
	mg_set_request_handler(mg, "/mjpg/video.mjpg", h_stream, ctx); /* legacy alias */
	mg_set_request_handler(mg, "/api/status", h_status, ctx);
	mg_set_request_handler(mg, "/api/login", h_login, ctx);
	mg_set_request_handler(mg, "/api/logout", h_logout, ctx);
	mg_set_request_handler(mg, "/api/config", h_config, ctx);
	mg_set_request_handler(mg, "/api/ota", h_ota, ctx);
	mg_set_request_handler(mg, "/api/reboot", h_reboot, ctx);
	mg_set_request_handler(mg, "/api/factory-reset", h_factory_reset, ctx);
	mg_set_request_handler(mg, "/api/password", h_password, ctx);
	mg_set_request_handler(mg, "/api/wifi/scan", h_wifi_scan, ctx);
	mg_set_request_handler(mg, "/api/wifi/test", h_wifi_test, ctx);
	mg_set_request_handler(mg, "/api/network/confirm", h_network, ctx);
	mg_set_request_handler(mg, "/api/network", h_network, ctx);
	mg_set_request_handler(mg, "/api/debug", h_debug, ctx);
	LOGI("http: serving on :%d (docroot %s)", port, docroot && docroot[0] ? docroot : "(none)");
	return mg;
}

void http_stop(struct mg_context *mg)
{
	if (mg)
		mg_stop(mg);
}
