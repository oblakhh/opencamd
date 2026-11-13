/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * wifi — Wi-Fi client control: validation, bring-up, link status, scan
 * and the "test connection" flow (see wifi.h).
 */

#include "wifi.h"
#include "log.h"
#include "wpa_drv.h"
#include "mbedtls/platform_util.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/sockios.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define WIFI_SCRIPT "/etc/wifiapply.sh"

int wifi_validate(const wifi_cfg *w, const char **err)
{
	size_t sl = strlen(w->ssid), pl = strlen(w->passphrase), i;
	const char *e = NULL;
	for (i = 0; i < sl; i++)
		if ((unsigned char)w->ssid[i] < 0x20 || w->ssid[i] == 0x7f) { e = "SSID contains control characters"; break; }
	for (i = 0; !e && i < pl; i++)
		if ((unsigned char)w->passphrase[i] < 0x20 || (unsigned char)w->passphrase[i] > 0x7e) { e = "passphrase must be printable ASCII"; break; }
	if (!e && w->enabled && (sl < 1 || sl > 32))
		e = "SSID must be 1-32 characters";
	else if (!e && w->enabled && (pl < 8 || pl > 63))
		e = "passphrase must be 8-63 characters";
	if (err)
		*err = e;
	return e ? -1 : 0;
}

#define TEST_MAC "02:00:00:00:00:01"   /* locally administered: never the eth0 MAC */

/* After it rejects a handshake, the FRITZ!Repeater refuses to start one with
 * ANY station for a while (associates, never sends msg-1): measured refused
 * 2/2 when retried at once, accepted 2/2 after a 30 s pause, also with a
 * fresh MAC - so it is AP-wide, not per-MAC. Tests are held off that long. */
#define T_COOLDOWN_S 30
static long g_reject_at = -1000;   /* uptime (s) of the last rejected test */

static long uptime_s(void)
{
	struct sysinfo si;
	return sysinfo(&si) == 0 ? (long)si.uptime : 0;
}

int wifi_test_cooldown(void)
{
	long left = g_reject_at + T_COOLDOWN_S - uptime_s();
	return left > 0 ? (int)left : 0;
}

/* Test thresholds (ms since the join was triggered), measured on the FRITZ!
 * APs (docs/firmware.md, Wi-Fi): correct passphrase -> port
 * open in a few s; wrong passphrase -> the AP re-sends msg-1 instead of
 * answering our msg-2 with msg-3 (wpa.c: wpa_wrong_passphrase); non-existent
 * SSID -> never associates. */
#define T_NOTFOUND_MS 18000   /* never associated, no msg-1 (after one re-join) */
#define T_JOIN_MS     22000   /* hard limit for the join phase */
#define T_REJOIN_MS    9000   /* not associated yet: re-join once */
#define T_DHCP_MS     10000   /* udhcpc -n gives up after ~8 s by itself */

static long now_ms(void)
{
	/* Monotonic: the wall clock jumps from 1970 to the present when NTP syncs, which
	 * made intervals across the jump meaningless (and tv_sec * 1000 overflowed). */
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)((unsigned long)ts.tv_sec * 1000UL + (unsigned long)(ts.tv_nsec / 1000000L));
}

/* Start the script with a minimal environment (PATH only); stdout -> *outfd. */
static pid_t spawn_script(char *const argv[], int *outfd)
{
	char *envp[] = { "PATH=/bin:/sbin:/usr/bin:/usr/sbin", NULL };
	int pfd[2] = { -1, -1 };
	pid_t p;
	if (outfd && pipe(pfd) != 0)
		return -1;
	p = fork();
	if (p < 0) {
		if (outfd) { close(pfd[0]); close(pfd[1]); }
		return -1;
	}
	if (p == 0) {
		if (outfd) { dup2(pfd[1], 1); close(pfd[0]); close(pfd[1]); }
		execve("/bin/sh", argv, envp);
		_exit(127);
	}
	if (outfd) { close(pfd[1]); *outfd = pfd[0]; }
	return p;
}

/* Run the script and wait; optionally capture stdout. */
static int run_script(char *const argv[], char *out, size_t outsz)
{
	int fd = -1, st = 0;
	size_t got = 0;
	pid_t p = spawn_script(argv, out ? &fd : NULL);
	if (p < 0)
		return -1;
	if (out) {
		ssize_t r;
		while (got + 1 < outsz && (r = read(fd, out + got, outsz - 1 - got)) > 0)
			got += (size_t)r;
		out[got] = '\0';
		close(fd);
	}
	waitpid(p, &st, 0);
	return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

/* PMK (cached; B5 errors) -> prep (down, MAC, board + station MIBs, SSID)
 * -> starttest (wlan0 up, CCK SIFS fix) -> supplicant: SET_RSNIE
 * starts the join. The passphrase never leaves camd: the driver only ever
 * sees the derived session keys. */
/* the driver reports the 802.1X port open (our supplicant finished the 4-way).
 * The driver's /proc handlers return NOTHING for a read shorter than ~1 KiB
 * (measured: 1023 -> 0 bytes, 1024 -> all 27 lines), so read 4 KiB at once;
 * 0.4.36 read 1023 bytes and never saw the port open (waited the full 15 s). */
static int proc_port_open(void)
{
	static char buf[4096];
	int fd = open("/proc/wlan0/sta_info", O_RDONLY), n = -1;
	if (fd >= 0) {
		n = (int)read(fd, buf, sizeof(buf) - 1);
		close(fd);
	}
	buf[n > 0 ? n : 0] = '\0';
	return strstr(buf, "ieee8021x_ctrlport: 1") != NULL;
}

int wifi_wait_port(int max_ms, int step_ms, int (*aborted)(void), int (*port_open)(void))
{
	int waited;
	for (waited = 0; waited < max_ms; waited += step_ms) {
		if (port_open())
			return 1;
		if (aborted && aborted())
			return -1;      /* net.c needs wlan0 / the cable is back: don't stall it */
		usleep((unsigned)step_ms * 1000u);
	}
	return 0;
}

static int bring_up(const char *ssid, const char *psk, const char *mac,
                    int uplink, const char *host, int (*aborted)(void))
{
	/* Order matters: with 802_1x=1 nothing joins until wpa_drv_start() issues
	 * SET_RSNIE, so DHCP must only start once our supplicant opened the port
	 * (starting it earlier blocked ~30 s and missed the handshake window). */
	char *pa[] = { "/bin/sh", WIFI_SCRIPT, "prep", (char *)ssid, (char *)(mac ? mac : ""), NULL };
	char *sa[] = { "/bin/sh", WIFI_SCRIPT, "starttest", NULL };
	char *da[] = { "/bin/sh", WIFI_SCRIPT, "dhcp", (char *)(host ? host : ""), NULL };
	unsigned char pmk[WPA_PMK_LEN];
	int rc = 0;
	if (wpa_drv_pmk(ssid, psk, pmk) != 0)
		return -1;
	wpa_drv_stop();
	if (run_script(pa, NULL, 0) != 0)
		rc = -1;
	if (run_script(sa, NULL, 0) != 0)
		rc = -1;
	if (wpa_drv_start(pmk) != 0)
		rc = -1;
	mbedtls_platform_zeroize(pmk, sizeof(pmk));
	if (rc == 0 && uplink) {
		int w = wifi_wait_port(15000, 250, aborted, proc_port_open);
		if (w == 0)
			LOGW("wifi: port not open after 15 s, starting DHCP anyway");
		else if (w < 0)
			LOGI("wifi: port wait aborted (uplink change pending), starting DHCP anyway");
		if (run_script(da, NULL, 0) != 0)
			rc = -1;
	}
	return rc;
}

static void wlan_down(void)
{
	char *argv[] = { "/bin/sh", WIFI_SCRIPT, "down", NULL };
	wpa_drv_stop();
	run_script(argv, NULL, 0);
}

int wifi_apply_sync(const wifi_cfg *w, const char *hostname, const char *mac, int (*aborted)(void))
{
	char host[64];
	int rc = 0;
	snprintf(host, sizeof(host), "%s", hostname && *hostname ? hostname : "ic3115w");
	if (w->enabled)
		rc = bring_up(w->ssid, w->passphrase, mac, 1, host, aborted);
	else {
		wlan_down();
		wpa_drv_forget();      /* Wi-Fi off: no PMK left in RAM (review L4) */
	}
	LOGI("wifi: %s %s%s", w->enabled ? "up" : "down", w->enabled ? w->ssid : "",
	     rc ? " (script failed)" : "");
	return rc;
}

static int field(const char *buf, const char *key, char *out, size_t n)
{
	const char *p = strstr(buf, key);
	size_t i = 0;
	if (!p)
		return 0;
	p += strlen(key);
	while (*p == ' ' || *p == '\t') p++;
	while (*p && *p != '\n' && *p != '\r' && i + 1 < n) out[i++] = *p++;
	out[i] = '\0';
	return 1;
}

static void slurp(const char *path, char *buf, size_t n)
{
	FILE *f = fopen(path, "r");
	size_t got = 0;
	buf[0] = '\0';
	if (!f) return;
	got = fread(buf, 1, n - 1, f);
	buf[got] = '\0';
	fclose(f);
}

int wifi_test_decide(int elapsed_ms, const wifi_obs *o)
{
	if (o->port_open)
		return WT_OK;
	if (o->wrong)                      /* the AP did not accept our msg-2 */
		return WT_WRONG_PASS;
	if (elapsed_ms >= T_NOTFOUND_MS && !o->assoc_seen && !o->msg1)
		return WT_NOT_FOUND;
	if (elapsed_ms >= T_JOIN_MS) {
		if (o->msg3_ok)                /* the AP accepted our msg-2: not the passphrase (L3) */
			return WT_ERROR;
		return o->msg1 ? WT_WRONG_PASS : o->assoc_seen ? WT_ERROR : WT_NOT_FOUND;
	}
	return WT_PENDING;
}

const char *wifi_test_name(int r)
{
	switch (r) {
	case WT_OK:         return "ok";
	case WT_WRONG_PASS: return "wrong_passphrase";
	case WT_NOT_FOUND:  return "not_found";
	case WT_NO_DHCP:    return "no_dhcp";
	case WT_BUSY:       return "busy";
	default:            return "error";
	}
}

/* One-shot DHCP lease check; returns 0 with ip set, -1 none, -2 aborted. */
static int dhcp_probe(int (*aborted)(void), char ip[16])
{
	char *argv[] = { "/bin/sh", WIFI_SCRIPT, "probe", NULL };
	char out[512];
	size_t got = 0;
	int fd = -1, rc = -1, st;
	long t0 = now_ms();
	pid_t p = spawn_script(argv, &fd);
	const char *k;
	if (p < 0)
		return -1;
	out[0] = '\0';
	for (;;) {
		fd_set rf;
		struct timeval tv;
		ssize_t r;
		if (aborted && aborted()) { rc = -2; break; }
		if (now_ms() - t0 > T_DHCP_MS)
			break;
		FD_ZERO(&rf);
		FD_SET(fd, &rf);
		tv.tv_sec = 0;
		tv.tv_usec = 250000;
		if (select(fd + 1, &rf, NULL, NULL, &tv) <= 0)
			continue;
		r = read(fd, out + got, sizeof(out) - 1 - got);
		if (r <= 0)
			break;                         /* udhcpc exited */
		got += (size_t)r;
		out[got] = '\0';
		if (strchr(out, '\n') && strstr(out, "DHCP_IP="))
			break;
		if (got + 1 >= sizeof(out))
			break;
	}
	close(fd);
	kill(p, SIGKILL);                          /* 'exec udhcpc' -> p is udhcpc */
	waitpid(p, &st, 0);
	if (rc != -2 && (k = strstr(out, "DHCP_IP=")) != NULL) {
		size_t i = 0;
		k += 8;
		while (i < 15 && ((k[i] >= '0' && k[i] <= '9') || k[i] == '.')) { ip[i] = k[i]; i++; }
		ip[i] = '\0';
		if (i >= 7)
			rc = 0;
	}
	return rc;
}

int wifi_test(const char *ssid, const char *psk, int dhcp, int (*aborted)(void),
              char ip[16], int *join_ms)
{
	char sta[2048];
	int r = WT_PENDING, el = 0, retried = 0;
	long t0;
	wifi_obs o;
	wpa_drv_info di;

	ip[0] = '\0';
	memset(&o, 0, sizeof(o));
	memset(&di, 0, sizeof(di));
	LOGI("wifi: test join '%s' (MAC " TEST_MAC ")", ssid);
	if (bring_up(ssid, psk, TEST_MAC, 0, NULL, NULL) != 0) {
		wlan_down();
		LOGW("wifi: test bring-up failed");
		return WT_ERROR;
	}
	t0 = now_ms();
	while (r == WT_PENDING) {
		if (aborted && aborted()) {
			r = WT_ERROR;
			break;
		}
		usleep(250000);
		el = (int)(now_ms() - t0);
		wpa_drv_get(&di);
		slurp("/proc/wlan0/sta_info", sta, sizeof(sta));
		if (di.assoc || strstr(sta, "(active: 1)"))
			o.assoc_seen = 1;
		/* our SET_PORT, confirmed by the driver's station entry */
		o.port_open = di.port && strstr(sta, "ieee8021x_ctrlport: 1") != NULL;
		o.msg1 = di.st.msg1;
		o.wrong = di.wrong;
		o.msg3_ok = di.st.msg3_ok;
		r = wifi_test_decide(el, &o);
		if (r == WT_PENDING && !retried && !o.msg1 && !di.assoc && el >= T_REJOIN_MS) {
			/* a scan occasionally misses the AP; a fresh join fixes it (docs/engineering-notes.md).
			 * "Associated, but no msg-1" is re-joined by wpa_drv.c. */
			retried = 1;
			wpa_drv_rejoin();
		}
	}
	if (join_ms)
		*join_ms = el;
	if (r == WT_OK && dhcp) {
		int d = dhcp_probe(aborted, ip);
		if (d == -2)
			r = WT_ERROR;
		else if (d != 0)
			r = WT_NO_DHCP;
	}
	wpa_drv_get(&di);
	wlan_down();                               /* never leave wlan0 up */
	if (o.msg1 && r != WT_OK)
		g_reject_at = uptime_s();          /* AP cool-down starts now */
	LOGI("wifi: test '%s' -> %s after %d ms (assoc %ld ms, msg-1 %ld ms x%u, msg-1 re-sent %u, "
	     "rejected %u, port %ld ms, GTK %ld ms, drops %u, joins %u)%s%s",
	     ssid, wifi_test_name(r), el, di.assoc_ms, di.msg1_ms, di.st.msg1, di.st.retried,
	     di.st.rejects, di.done_ms, di.gtk_ms, di.st.drops, di.joins,
	     ip[0] ? ", lease " : "", ip);
	return r;
}

void wifi_get_status(const wifi_cfg *w, wifi_status *s)
{
	char sta[2048], cfg[2048], v[64];
	struct ifreq ifr;
	int fd;

	memset(s, 0, sizeof(*s));
	s->rssi = -1;
	s->state = "off";
	if (!w->enabled)
		return;
	snprintf(s->ssid, sizeof(s->ssid), "%s", w->ssid);
	s->state = "standby";   /* enabled, but wlan0 down: cable is the uplink */
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd >= 0) {
		memset(&ifr, 0, sizeof(ifr));
		snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "wlan0");
		if (ioctl(fd, SIOCGIFFLAGS, &ifr) != 0 || !(ifr.ifr_flags & IFF_UP)) {
			close(fd);
			return;
		}
		close(fd);
	}
	slurp("/proc/wlan0/sta_info", sta, sizeof(sta));
	slurp("/proc/wlan0/mib_staconfig", cfg, sizeof(cfg));
	s->state = "connecting";
	if (strstr(sta, "(active: 1)"))
		s->state = "associated";
	if (strstr(sta, "ieee8021x_ctrlport: 1")) {
		s->state = "connected";
	} else {
		wpa_drv_info di;
		wpa_drv_get(&di);
		if (di.running && di.wrong)   /* the AP keeps refusing our msg-2 */
			s->state = "wrong_passphrase";
	}
	if (field(sta, "rssi:", v, sizeof(v)))
		s->rssi = atoi(v);
	if (field(cfg, "dot11Bssid:", v, sizeof(v)) && strlen(v) == 12) {
		int i;
		for (i = 0; i < 6; i++)
			snprintf(s->bssid + i * 3, 4, "%c%c%s", v[i*2], v[i*2+1], i < 5 ? ":" : "");
	}
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd >= 0) {
		memset(&ifr, 0, sizeof(ifr));
		snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "wlan0");
		if (ioctl(fd, SIOCGIFADDR, &ifr) == 0) {
			struct sockaddr_in *sa = (struct sockaddr_in *)&ifr.ifr_addr;
			if (sa->sin_addr.s_addr)
				snprintf(s->ip, sizeof(s->ip), "%s", inet_ntoa(sa->sin_addr));
		}
		close(fd);
	}
}

cJSON *wifi_scan(void)
{
	static char buf[8192];
	char *argv[] = { "/bin/sh", WIFI_SCRIPT, "scan", NULL };
	cJSON *arr = cJSON_CreateArray(), *cur = NULL;
	char *line, *save = NULL;

	if (run_script(argv, buf, sizeof(buf)) != 0)
		LOGW("wifi: scan script failed");
	for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char v[64];
		while (*line == ' ') line++;
		if (strncmp(line, "HwAddr:", 7) == 0) {
			cur = cJSON_CreateObject();
			cJSON_AddItemToArray(arr, cur);
			field(line, "HwAddr:", v, sizeof(v));
			cJSON_AddStringToObject(cur, "bssid", v);
		} else if (cur && strncmp(line, "Channel:", 8) == 0) {
			field(line, "Channel:", v, sizeof(v));
			cJSON_AddNumberToObject(cur, "channel", atoi(v));
		} else if (cur && strncmp(line, "SSID:", 5) == 0) {
			field(line, "SSID:", v, sizeof(v));
			cJSON_AddStringToObject(cur, "ssid", v);
		}
	}
	return arr;
}
