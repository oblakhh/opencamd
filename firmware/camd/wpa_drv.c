/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * wpa_drv — drives the wpa.c supplicant through the RTL8192CD driver's
 * userland-supplicant ioctls (see wpa_drv.h).
 */

#include "wpa_drv.h"
#include "log.h"

#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <linux/if_packet.h>
#include <linux/sockios.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"

#define IFNAME "wlan0"

/* ---- driver interface (8192cd_security.h / 8192cd_ioctl.c) -----------------
 * RTL8192CD_IOCTL_USER_DAEMON_REQUEST = SIOCDEVPRIVATE + 0xf = 0x89ff, an
 * iwreq whose u.data.pointer is a DOT11_* struct starting with EventId. The
 * driver reads the request in place (INCLUDE_WPA_PSK build) and returns 0
 * for every request, so success has to be verified in /proc. Structs below
 * reproduce the kernel's MIPS32 layout (unsigned long = 4 bytes, natural
 * alignment, big-endian native fields); sizes are checked at compile time. */
#define RTL_DAEMON_REQ (SIOCDEVPRIVATE + 0xf)
#define MAXDATALEN     1560    /* 8192cd_cfg.h: largest queue item */

enum {
	EV_NO_EVENT = 1, EV_REQUEST = 2, EV_DISASSOC_IND = 8, EV_SET_KEY = 11, EV_SET_PORT = 12,
	EV_SET_RSNIE = 14, EV_MIC_FAILURE = 16, EV_INIT_QUEUE = 18, EV_STA_QUERY_BSSID = 36,
	EV_EAP_PACKET = 41
};
enum { KEYTYPE_GROUP = 0, KEYTYPE_PAIRWISE = 1 };
#define ENC_CCMP 4

struct d_set_key {             /* DOT11_SET_KEY */
	uint8_t  EventId, IsMoreEvent;
	uint32_t KeyIndex;
	uint32_t KeyLen;
	uint8_t  KeyType, EncType;
	uint8_t  MACAddr[6];
	uint8_t  KeyRSC[8];
	uint8_t  KeyMaterial[64];
};
struct d_set_rsnie {           /* DOT11_SET_RSNIE */
	uint8_t  EventId, IsMoreEvent;
	uint16_t Flag;             /* 1 = DOT11_Ioctl_Set */
	uint16_t RSNIELen;
	uint8_t  RSNIE[128];
	uint8_t  MACAddr[6];
};
struct d_set_port {            /* DOT11_SET_PORT */
	uint8_t  EventId, PortStatus, PortType;
	uint8_t  MACAddr[6];
};
struct d_query_bssid {         /* DOT11_STA_QUERY_BSSID */
	uint8_t  EventId, IsMoreEvent;
	uint32_t IsValid;
	uint8_t  Bssid[6];
};
struct d_simple {              /* DOT11_REQUEST / DOT11_INIT_QUEUE */
	uint8_t  EventId, IsMoreEvent;
};
/* DOT11_EAP_PACKET: EventId, IsMoreEvent, u16 packet_len, packet[1550] =
 * Ethernet header + EAPOL frame */
#define EAP_HDR 4

typedef char chk_set_key[sizeof(struct d_set_key) == 92 ? 1 : -1];
typedef char chk_set_rsnie[sizeof(struct d_set_rsnie) == 140 ? 1 : -1];
typedef char chk_set_port[sizeof(struct d_set_port) == 9 ? 1 : -1];
typedef char chk_query[sizeof(struct d_query_bssid) == 16 ? 1 : -1];
typedef char chk_key_off[offsetof(struct d_set_key, KeyIndex) == 4 && offsetof(struct d_set_key, KeyType) == 12 &&
                         offsetof(struct d_set_key, MACAddr) == 14 && offsetof(struct d_set_key, KeyRSC) == 20 &&
                         offsetof(struct d_set_key, KeyMaterial) == 28 ? 1 : -1];
typedef char chk_rsnie_off[offsetof(struct d_set_rsnie, RSNIELen) == 4 && offsetof(struct d_set_rsnie, RSNIE) == 6 &&
                           offsetof(struct d_set_rsnie, MACAddr) == 134 ? 1 : -1];
typedef char chk_query_off[offsetof(struct d_query_bssid, IsValid) == 4 && offsetof(struct d_query_bssid, Bssid) == 8 ? 1 : -1];

struct rtl_iwreq {             /* struct iwreq: name + iw_point */
	char ifname[IFNAMSIZ];
	struct { void *pointer; unsigned short length, flags; } data;
	char pad[8];
};

/* ---- state ------------------------------------------------------------- */
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thr;
static volatile int g_stop;
static int g_running;
static int g_ioc = -1, g_pk = -1, g_ifindex;
static wpa_sm g_sm;
static int g_port;
static long g_t0, g_assoc_ms, g_msg1_ms, g_done_ms, g_gtk_ms, g_join_ms;
static long g_assoc_at;          /* start of the current association */
static long g_nomsg1_ms;         /* current "associated, no msg-1" re-join delay */
static unsigned g_joins;
static int g_m1_rc_set;
static unsigned char g_m1_rc[WPA_RC_LEN];

static mbedtls_entropy_context g_ent;
static mbedtls_ctr_drbg_context g_drbg;
static int g_drbg_ok;

static int g_ptk_pend;           /* rekey: new TK waiting for msg-4 to leave (review M2) */
static long g_ptk_pend_at;
static unsigned char g_ptk_pend_tk[WPA_TK_LEN];

#ifdef WPA_DRV_TEST
/* host-test hooks (tests/test_wpa.c): no driver, no sockets, no clock */
int (*wpa_drv_t_req)(void *p, size_t len);
int (*wpa_drv_t_ctrlport)(void);
int (*wpa_drv_t_send)(const unsigned char *f, size_t len);
long wpa_drv_t_now;
unsigned wpa_drv_t_logs;
#define DLOG(lvl, ...) do { wpa_drv_t_logs++; syslog(lvl, __VA_ARGS__); } while (0)
#else
#define DLOG(lvl, ...) syslog(lvl, __VA_ARGS__)
#endif

static long now_ms(void)
{
#ifdef WPA_DRV_TEST
	return wpa_drv_t_now;
#else
	/* Monotonic: the wall clock jumps from 1970 to the present when NTP syncs, which
	 * broke join timeouts and logged "port open at -8179... ms" (tv_sec * 1000 overflowed). */
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)((unsigned long)ts.tv_sec * 1000UL + (unsigned long)(ts.tv_nsec / 1000000L));
#endif
}

/* Log lines that frames from the air can trigger (drops, msg-1, re-association
 * heuristics) are rate-limited per kind: at most RL_MAX per RL_WINDOW_MS, then
 * one summary line, so a forger cannot flood the (remote) syslog (review L2). */
#define RL_WINDOW_MS 10000
#define RL_MAX       5
typedef struct { long t0; unsigned n, suppressed; } ratelimit;
static ratelimit g_rl_drop, g_rl_msg1, g_rl_reassoc;

static int rl_allow(ratelimit *r, const char *what)
{
	long now = now_ms();
	if (r->n == 0 || now - r->t0 >= RL_WINDOW_MS) {
		if (r->suppressed)
			DLOG(LOG_WARNING, "wpa: %u more '%s' lines suppressed", r->suppressed, what);
		r->t0 = now;
		r->n = 0;
		r->suppressed = 0;
	}
	if (r->n < RL_MAX) {
		r->n++;
		return 1;
	}
	r->suppressed++;
	return 0;
}

static int req(void *p, size_t len)
{
	struct rtl_iwreq w;
#ifdef WPA_DRV_TEST
	if (wpa_drv_t_req)
		return wpa_drv_t_req(p, len);
#endif
	memset(&w, 0, sizeof(w));
	snprintf(w.ifname, sizeof(w.ifname), IFNAME);
	w.data.pointer = p;
	w.data.length = (unsigned short)len;
	return ioctl(g_ioc, RTL_DAEMON_REQ, &w);
}

static int query_bssid(unsigned char b[6])
{
	struct d_query_bssid q;
	memset(&q, 0, sizeof(q));
	q.EventId = EV_STA_QUERY_BSSID;
	if (req(&q, sizeof(q)) != 0 || !q.IsValid)
		return 0;
	memcpy(b, q.Bssid, 6);
	return 1;
}

static int set_rsnie(void)
{
	struct d_set_rsnie r;
	unsigned char ie[22];
	memset(&r, 0, sizeof(r));
	r.EventId = EV_SET_RSNIE;
	r.Flag = 1;
	r.RSNIELen = (uint16_t)wpa_rsn_ie(ie);
	memcpy(r.RSNIE, ie, r.RSNIELen);
	return req(&r, sizeof(r));
}

static int set_key(int type, const unsigned char mac[6], int idx, const unsigned char *key,
                   const unsigned char rsc[8])
{
	struct d_set_key k;
	int rc;
	memset(&k, 0, sizeof(k));
	k.EventId = EV_SET_KEY;
	k.KeyIndex = (uint32_t)idx;
	k.KeyLen = 16;
	k.KeyType = (uint8_t)type;
	k.EncType = ENC_CCMP;
	memcpy(k.MACAddr, mac, 6);
	if (rsc)
		memcpy(k.KeyRSC, rsc, 8);   /* carried, but this driver does not use it */
	memcpy(k.KeyMaterial, key, 16);
	rc = req(&k, sizeof(k));
	mbedtls_platform_zeroize(&k, sizeof(k));
	return rc;
}

static int set_port(const unsigned char mac[6], int open)
{
	struct d_set_port p;
	memset(&p, 0, sizeof(p));
	p.EventId = EV_SET_PORT;
	p.PortStatus = (uint8_t)(open ? 1 : 0);
	memcpy(p.MACAddr, mac, 6);
	return req(&p, sizeof(p));
}

static void slurp(const char *path, char *buf, size_t n)
{
	FILE *f = fopen(path, "r");
	size_t got = 0;
	buf[0] = '\0';
	if (!f)
		return;
	got = fread(buf, 1, n - 1, f);
	buf[got] = '\0';
	fclose(f);
}

/* the driver's controlled-port flag of the AP's station entry; a fresh
 * entry (new association) starts at 0 */
static int drv_ctrlport(void)
{
	char b[2048];
#ifdef WPA_DRV_TEST
	if (wpa_drv_t_ctrlport)
		return wpa_drv_t_ctrlport();
#endif
	slurp("/proc/" IFNAME "/sta_info", b, sizeof(b));
	return strstr(b, "ieee8021x_ctrlport: 1") != NULL;
}

/* The driver returns 0 for every request, so key installs are checked in
 * /proc (only the keyInCam flags are logged, never key bytes). */
static void log_keys(int pairwise, int group)
{
	char b[2048];
	int pw = -1, gk = -1;
#ifdef WPA_DRV_TEST
	return;
#endif
	if (pairwise) {
		slurp("/proc/" IFNAME "/sta_keyinfo", b, sizeof(b));
		pw = strstr(b, "keyInCam: yes") != NULL;
	}
	if (group) {
		slurp("/proc/" IFNAME "/mib_gkeytbl", b, sizeof(b));
		gk = strstr(b, "keyInCam: yes") != NULL;
	}
	mbedtls_platform_zeroize(b, sizeof(b));
	if (pw == 0 || gk == 0)
		LOGW("wpa: key install NOT confirmed by the driver (pairwise %d, group %d)", pw, gk);
	else
		LOGI("wpa: driver confirms keys in CAM (pairwise %s, group %s)",
		     pw < 0 ? "-" : "yes", gk < 0 ? "-" : "yes");
}

static int send_eapol(const unsigned char *f, size_t len)
{
	unsigned char fr[14 + WPA_TX_MAX];
	struct sockaddr_ll sll;
	if (len > WPA_TX_MAX)
		return -1;
#ifdef WPA_DRV_TEST
	if (wpa_drv_t_send)
		return wpa_drv_t_send(f, len);
#endif
	memcpy(fr, g_sm.aa, 6);
	memcpy(fr + 6, g_sm.spa, 6);
	fr[12] = 0x88;
	fr[13] = 0x8e;
	memcpy(fr + 14, f, len);
	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_ifindex = g_ifindex;
	sll.sll_protocol = htons(0x888e);
	sll.sll_halen = 6;
	memcpy(sll.sll_addr, g_sm.aa, 6);
	if (sendto(g_pk, fr, 14 + len, 0, (struct sockaddr *)&sll, sizeof(sll)) != (ssize_t)(14 + len)) {
		LOGW("wpa: EAPOL send failed: %s", strerror(errno));
		return -1;
	}
	return 0;
}

static int drbg_rng(void *ctx, unsigned char *b, size_t n)
{
	return mbedtls_ctr_drbg_random(ctx, b, n) == 0 ? 0 : -1;
}

static const char *fmt_mac(const unsigned char *m, char *o)
{
	snprintf(o, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
	return o;
}

/* ---- event loop (called with g_mx held) ------------------------------- */

struct d_delete_key {          /* DOT11_DELETE_KEY */
	uint8_t EventId, IsMoreEvent;
	uint8_t MACAddr[6];
	uint8_t KeyType;
};

/* The association is gone or changed: forget a pending rekey TK and drop the
 * group key, so a stale CAM entry can't "confirm" the next install
 * (review L7). DOT11_DELETE_KEY(group) zeroes the driver's group-key length
 * (8192cd_security.c:336-356); the next SET_KEY sets it again. */
static void assoc_gone(void)
{
	struct d_delete_key d;
	g_port = 0;
	g_m1_rc_set = 0;
	g_ptk_pend = 0;
	mbedtls_platform_zeroize(g_ptk_pend_tk, sizeof(g_ptk_pend_tk));
	g_gtk_ms = -1;
	memset(&d, 0, sizeof(d));
	d.EventId = 13;
	d.KeyType = KEYTYPE_GROUP;
	memset(d.MACAddr, 0xff, 6);
	req(&d, sizeof(d));
}

static void on_assoc_change(void)
{
	unsigned char b[6];
	char ms[18];
	int v = query_bssid(b);
	if (v && (!g_sm.assoc || memcmp(b, g_sm.aa, 6) != 0)) {
		if (g_sm.assoc)
			assoc_gone();      /* roam: not counted as a rejection */
		wpa_assoc(&g_sm, b);
		g_port = 0;
		g_m1_rc_set = 0;
		g_assoc_at = now_ms();
		if (g_assoc_ms < 0)
			g_assoc_ms = now_ms() - g_t0;
		LOGI("wpa: associated with %s", fmt_mac(b, ms));
	} else if (!v && g_sm.assoc) {
		unsigned rj = g_sm.st.rejects;
		wpa_disassoc(&g_sm);   /* the only place a rejection is counted */
		assoc_gone();
		LOGI("wpa: association lost%s", g_sm.st.rejects != rj ? " (our msg-2 was never answered)" : "");
	}
}

/* A heuristic (driven by unauthenticated msg-1 frames) says the AP started
 * a new association: reset, but never count a rejection (review M1). */
static void restart_assoc(const char *why)
{
	if (rl_allow(&g_rl_reassoc, "re-association detected"))
		DLOG(LOG_INFO, "wpa: re-association detected (%s)", why);
	wpa_reassoc(&g_sm);
	g_port = 0;
	g_m1_rc_set = 0;
	g_ptk_pend = 0;
	mbedtls_platform_zeroize(g_ptk_pend_tk, sizeof(g_ptk_pend_tk));
}

static void on_eapol(const unsigned char *pkt, size_t plen)
{
	const unsigned char *src = pkt + 6, *f = pkt + 14;
	size_t flen;
	wpa_out o;
	char ms[18];
	int r;

	if (plen < 14 + 4 || pkt[12] != 0x88 || pkt[13] != 0x8e)
		return;
	flen = plen - 14;
	if (!g_sm.assoc)
		on_assoc_change();       /* msg-1 can be queued before our poll saw the association */
	/* new association that the 60 ms poll missed (deauth + quick re-join):
	 * a pairwise msg-1 while the driver's fresh station entry has its port
	 * closed although we opened it, or a msg-1 replay counter going backwards
	 * before any MIC-verified frame (a new authenticator session) */
	if (g_sm.assoc && flen >= WPA_OFF_KD && f[1] == 3 &&
	    (((f[WPA_OFF_INFO] << 8) | f[WPA_OFF_INFO + 1]) & (WPA_KI_PAIRWISE | WPA_KI_ACK | WPA_KI_MIC)) ==
	    (WPA_KI_PAIRWISE | WPA_KI_ACK) && memcmp(src, g_sm.aa, 6) == 0) {
		if (g_sm.state == WPA_DONE && g_port && !drv_ctrlport())
			restart_assoc("port closed by the driver");
		else if (!g_sm.rc_set && g_m1_rc_set && memcmp(f + WPA_OFF_RC, g_m1_rc, WPA_RC_LEN) < 0)
			restart_assoc("msg-1 replay counter went back");
		memcpy(g_m1_rc, f + WPA_OFF_RC, WPA_RC_LEN);
		g_m1_rc_set = 1;
	}
	r = wpa_rx(&g_sm, src, f, flen, &o);
	if (r != 0) {
		if (rl_allow(&g_rl_drop, "dropped EAPOL frame"))
			DLOG(LOG_WARNING, "wpa: dropped EAPOL frame from %s: %s", fmt_mac(src, ms), g_sm.last_drop);
		wpa_out_clear(&o);
		return;
	}
	if (o.tx_len)
		send_eapol(o.tx, o.tx_len);
	if (o.set_ptk && o.ptk_rekey) {
		/* PTK rekey (review M2): msg-4 was just queued encrypted under the old
		 * TK; swapping the CAM entry now could re-key it in the TX ring and the
		 * AP could not decrypt it. Install on a later tick (>= REKEY_DELAY_MS). */
		memcpy(g_ptk_pend_tk, o.tk, WPA_TK_LEN);
		g_ptk_pend = 1;
		g_ptk_pend_at = now_ms();
	} else if (o.set_ptk && set_key(KEYTYPE_PAIRWISE, g_sm.aa, 0, o.tk, NULL) != 0) {
		LOGW("wpa: SET_KEY (pairwise) failed: %s", strerror(errno));
	}
	if (o.set_gtk) {
		if (set_key(KEYTYPE_GROUP, (const unsigned char *)"\xff\xff\xff\xff\xff\xff", o.gtk_idx, o.gtk, o.rsc) != 0)
			LOGW("wpa: SET_KEY (group) failed: %s", strerror(errno));
		g_gtk_ms = now_ms() - g_t0;
	}
	if (o.open_port) {
		if (set_port(g_sm.aa, 1) != 0)
			LOGW("wpa: SET_PORT failed: %s", strerror(errno));
		g_port = 1;
		g_done_ms = now_ms() - g_t0;
	}
	switch (o.msg) {
	case 1:
		if (g_msg1_ms < 0)
			g_msg1_ms = now_ms() - g_t0;
		if (rl_allow(&g_rl_msg1, "msg-1 -> msg-2"))
			DLOG(LOG_INFO, "wpa: msg-1 -> msg-2 (msg-1 #%u, re-sent after our msg-2: %u)",
			     g_sm.st.msg1, g_sm.st.retried);
		break;
	case 3:
		{
			char l[80];
			size_t n = 0;
			int k;
			l[0] = '\0';
			for (k = 0; k < g_sm.kd_n && n + 9 < sizeof(l); k++)
				n += (size_t)snprintf(l + n, sizeof(l) - n, g_sm.kd_ids[k] >= 0xdd00 ? " KDE:%u" : " %02x",
				                      g_sm.kd_ids[k] >= 0xdd00 ? g_sm.kd_ids[k] & 0xff : g_sm.kd_ids[k]);
			LOGI("wpa: msg-3 key data elements:%s", l);
		}
		if (o.set_ptk && o.ptk_rekey)
			LOGI("wpa: PTK rekey: msg-3 -> msg-4, new PTK installed after msg-4 has left");
		else if (o.set_ptk)
			LOGI("wpa: msg-3 -> msg-4, PTK + GTK (key %d%s) installed, port open at %ld ms",
			     g_sm.gtk_idx, o.set_gtk ? "" : ", unchanged", g_done_ms);
		else
			LOGI("wpa: retransmitted msg-3 -> msg-4 again, nothing reinstalled");
		break;
	case 5:
		LOGI("wpa: group rekey -> GTK key %d %s", g_sm.gtk_idx, o.set_gtk ? "installed" : "unchanged");
		break;
	}
	if ((o.set_ptk && !o.ptk_rekey) || o.set_gtk)
		log_keys(o.set_ptk && !o.ptk_rekey, o.set_gtk);
	wpa_out_clear(&o);
}

static void drain(void)
{
	static unsigned char ev[MAXDATALEN];
	int i;
	for (i = 0; i < 8; i++) {
		struct rtl_iwreq w;
		unsigned short plen;
		memset(ev, 0, EAP_HDR);
		ev[0] = EV_REQUEST;
		memset(&w, 0, sizeof(w));
		snprintf(w.ifname, sizeof(w.ifname), IFNAME);
		w.data.pointer = ev;
		w.data.length = sizeof(ev);
		if (ioctl(g_ioc, RTL_DAEMON_REQ, &w) != 0 || ev[0] == EV_NO_EVENT)
			return;
		switch (ev[0]) {
		case EV_EAP_PACKET:
			memcpy(&plen, ev + 2, 2);        /* native (big-endian) u16 */
			if (plen <= sizeof(ev) - EAP_HDR && plen <= w.data.length - EAP_HDR)
				on_eapol(ev + EAP_HDR, plen);
			break;
		case EV_MIC_FAILURE:
			if (rl_allow(&g_rl_drop, "dropped EAPOL frame"))
				DLOG(LOG_WARNING, "wpa: driver reports a MIC failure (TKIP only; ignored with CCMP)");
			break;
		case EV_DISASSOC_IND:
			on_assoc_change();
			break;
		default:
			break;
		}
		mbedtls_platform_zeroize(ev, sizeof(ev));
	}
}

#define TICK_MS      20
#define WATCHDOG_MS  60000    /* not connected this long -> restart the join */
/* The FRITZ! APs sometimes associate us and never send msg-1 (normally it
 * follows the association within ~30 ms): measured 1 of 10 test joins, and
 * always during their ~30 s cool-down after a rejected handshake. A fresh
 * join fixes the former; back off 4 s, 8 s, ... 60 s for the latter. */
#define NOMSG1_MS     4000

static void rejoin_locked(const char *why)
{
	LOGI("wpa: %s -> restarting the join", why);
	g_join_ms = now_ms();
	g_joins++;
	set_rsnie();
}

#define REKEY_DELAY_MS 50

/* everything one tick does after the event queue was drained */
static void timers(void)
{
	if (g_ptk_pend && now_ms() - g_ptk_pend_at >= REKEY_DELAY_MS) {
		g_ptk_pend = 0;
		if (set_key(KEYTYPE_PAIRWISE, g_sm.aa, 0, g_ptk_pend_tk, NULL) != 0)
			LOGW("wpa: SET_KEY (pairwise rekey) failed: %s", strerror(errno));
		mbedtls_platform_zeroize(g_ptk_pend_tk, sizeof(g_ptk_pend_tk));
		log_keys(1, 0);
	}
	if (g_port) {
		g_nomsg1_ms = NOMSG1_MS;
	} else if (g_sm.assoc && g_sm.state == WPA_IDLE && now_ms() - g_assoc_at > g_nomsg1_ms &&
	           now_ms() - g_join_ms > g_nomsg1_ms) {
		rejoin_locked("associated, but the AP sent no msg-1");
		if (g_nomsg1_ms < WATCHDOG_MS)
			g_nomsg1_ms *= 2;
	} else if (now_ms() - g_join_ms > WATCHDOG_MS) {
		rejoin_locked("not connected for 60 s");
	}
}

static void *loop(void *arg)
{
	unsigned tick = 0;
	(void)arg;
	while (!g_stop) {
		pthread_mutex_lock(&g_mx);
		if (tick % 3 == 0)
			on_assoc_change();
		drain();
		timers();
		pthread_mutex_unlock(&g_mx);
		usleep(TICK_MS * 1000);
		tick++;
	}
	return NULL;
}

/* ---- public ------------------------------------------------------------- */

static pthread_mutex_t g_pmk_mx = PTHREAD_MUTEX_INITIALIZER;
static struct { int ok; unsigned char id[32]; unsigned char pmk[WPA_PMK_LEN]; } g_pmk;

int wpa_drv_pmk(const char *ssid, const char *passphrase, unsigned char pmk[WPA_PMK_LEN])
{
	unsigned char id[32];
	mbedtls_sha256_context h;
	int rc = 0;
	long t;

	if (!ssid || !passphrase)
		return -1;
	mbedtls_sha256_init(&h);
	mbedtls_sha256_starts_ret(&h, 0);
	mbedtls_sha256_update_ret(&h, (const unsigned char *)ssid, strlen(ssid) + 1);
	mbedtls_sha256_update_ret(&h, (const unsigned char *)passphrase, strlen(passphrase));
	mbedtls_sha256_finish_ret(&h, id);
	mbedtls_sha256_free(&h);
	pthread_mutex_lock(&g_pmk_mx);
	if (g_pmk.ok && memcmp(g_pmk.id, id, sizeof(id)) == 0) {
		memcpy(pmk, g_pmk.pmk, WPA_PMK_LEN);
	} else {
		t = now_ms();
		rc = wpa_pmk(passphrase, (const unsigned char *)ssid, strlen(ssid), pmk);
		mbedtls_platform_zeroize(&g_pmk, sizeof(g_pmk));
		if (rc == 0) {
			g_pmk.ok = 1;
			memcpy(g_pmk.id, id, sizeof(id));
			memcpy(g_pmk.pmk, pmk, WPA_PMK_LEN);
			LOGI("wpa: PMK derived in %ld ms", now_ms() - t);
		} else {
			LOGW("wpa: PMK derivation failed (invalid passphrase or SSID)");
		}
	}
	pthread_mutex_unlock(&g_pmk_mx);
	mbedtls_platform_zeroize(id, sizeof(id));
	return rc;
}

static void close_all(void)
{
	if (g_ioc >= 0)
		close(g_ioc);
	if (g_pk >= 0)
		close(g_pk);
	g_ioc = g_pk = -1;
}

int wpa_drv_start(const unsigned char pmk[WPA_PMK_LEN])
{
	struct ifreq ifr;
	unsigned char mac[6];
	struct d_simple q;

	wpa_drv_stop();
	if (!g_drbg_ok) {
		mbedtls_entropy_init(&g_ent);
		mbedtls_ctr_drbg_init(&g_drbg);
		if (mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_ent,
		                          (const unsigned char *)"camd-wpa", 8) != 0) {
			mbedtls_ctr_drbg_free(&g_drbg);
			mbedtls_entropy_free(&g_ent);
			LOGE("wpa: DRBG seeding failed");
			return -1;
		}
		g_drbg_ok = 1;
	}
	g_ioc = socket(AF_INET, SOCK_DGRAM, 0);
	g_pk = socket(PF_PACKET, SOCK_RAW, 0);     /* protocol 0: send only, receives nothing */
	if (g_ioc < 0 || g_pk < 0) {
		LOGE("wpa: sockets: %s", strerror(errno));
		close_all();
		return -1;
	}
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), IFNAME);
	if (ioctl(g_ioc, SIOCGIFINDEX, &ifr) != 0) {
		close_all();
		return -1;
	}
	g_ifindex = ifr.ifr_ifindex;
	if (ioctl(g_ioc, SIOCGIFHWADDR, &ifr) != 0) {
		close_all();
		return -1;
	}
	memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);

	pthread_mutex_lock(&g_mx);
	wpa_init(&g_sm, pmk, mac, drbg_rng, &g_drbg);
	g_port = 0;
	g_m1_rc_set = 0;
	g_t0 = g_join_ms = g_assoc_at = now_ms();
	g_assoc_ms = g_msg1_ms = g_done_ms = g_gtk_ms = -1;
	g_nomsg1_ms = NOMSG1_MS;
	g_joins = 1;
	memset(&q, 0, sizeof(q));
	q.EventId = EV_INIT_QUEUE;                 /* drop stale events */
	req(&q, sizeof(q));
	if (set_rsnie() != 0) {                    /* starts the join (8192cd_security.c) */
		LOGE("wpa: SET_RSNIE failed: %s", strerror(errno));
		wpa_clear(&g_sm);
		pthread_mutex_unlock(&g_mx);
		close_all();
		return -1;
	}
	g_stop = 0;
	if (pthread_create(&g_thr, NULL, loop, NULL) != 0) {
		wpa_clear(&g_sm);
		pthread_mutex_unlock(&g_mx);
		close_all();
		return -1;
	}
	g_running = 1;
	pthread_mutex_unlock(&g_mx);
	LOGI("wpa: supplicant started, join triggered");
	return 0;
}

void wpa_drv_stop(void)
{
	if (!g_running)
		return;
	g_stop = 1;
	pthread_join(g_thr, NULL);
	pthread_mutex_lock(&g_mx);
	g_running = 0;
	wpa_clear(&g_sm);
	g_port = 0;
	g_ptk_pend = 0;
	mbedtls_platform_zeroize(g_ptk_pend_tk, sizeof(g_ptk_pend_tk));
	close_all();
	pthread_mutex_unlock(&g_mx);
}

int wpa_drv_rejoin(void)
{
	int rc = -1;
	pthread_mutex_lock(&g_mx);
	if (g_running) {
		g_join_ms = now_ms();
		g_joins++;
		rc = set_rsnie();
		LOGI("wpa: join restarted");
	}
	pthread_mutex_unlock(&g_mx);
	return rc;
}

void wpa_drv_get(wpa_drv_info *i)
{
	memset(i, 0, sizeof(*i));
	pthread_mutex_lock(&g_mx);
	i->running = g_running;
	if (g_running) {
		i->assoc = g_sm.assoc;
		memcpy(i->bssid, g_sm.aa, 6);
		i->port = g_port;
		i->state = g_sm.state;
		i->st = g_sm.st;
		i->wrong = wpa_wrong_passphrase(&g_sm.st);
		i->up_ms = now_ms() - g_t0;
		i->assoc_ms = g_assoc_ms;
		i->msg1_ms = g_msg1_ms;
		i->done_ms = g_done_ms;
		i->gtk_ms = g_gtk_ms;
		i->joins = g_joins;
	}
	pthread_mutex_unlock(&g_mx);
}

void wpa_drv_forget(void)
{
	pthread_mutex_lock(&g_pmk_mx);
	mbedtls_platform_zeroize(&g_pmk, sizeof(g_pmk));
	pthread_mutex_unlock(&g_pmk_mx);
}

#ifdef WPA_DRV_TEST
int wpa_drv_t_pmk_cached(void)
{
	return g_pmk.ok;
}

void wpa_drv_t_setup(const unsigned char pmk[WPA_PMK_LEN], const unsigned char mac[6])
{
	wpa_init(&g_sm, pmk, mac, wpa_drv_t_rng, NULL);
	g_running = 1;
	g_port = 0;
	g_m1_rc_set = 0;
	g_ptk_pend = 0;
	g_t0 = g_join_ms = g_assoc_at = now_ms();
	g_assoc_ms = g_msg1_ms = g_done_ms = g_gtk_ms = -1;
	g_nomsg1_ms = NOMSG1_MS;
	memset(&g_rl_drop, 0, sizeof(g_rl_drop));
	memset(&g_rl_msg1, 0, sizeof(g_rl_msg1));
	memset(&g_rl_reassoc, 0, sizeof(g_rl_reassoc));
}
void wpa_drv_t_eapol(const unsigned char *pkt, size_t len) { on_eapol(pkt, len); }
void wpa_drv_t_poll(void) { on_assoc_change(); }
void wpa_drv_t_timers(void) { timers(); }
wpa_sm *wpa_drv_t_sm(void) { return &g_sm; }
int wpa_drv_t_port(void) { return g_port; }
#endif
