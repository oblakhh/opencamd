/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * wpa_drv — runs the wpa.c supplicant against the RTL8192CD driver's
 * userland-supplicant interface (802_1x=1, psk_enable=0):
 *   - events (EAPOL frames) are read from the driver queue with the
 *     USER_DAEMON_REQUEST ioctl (0x89ff, DOT11_EVENT_REQUEST), polled;
 *   - EAPOL is sent on a PF_PACKET socket on wlan0 (ethertype 0x888E);
 *   - the join is triggered with DOT11_EVENT_SET_RSNIE, keys go in with
 *     DOT11_EVENT_SET_KEY, the port opens with DOT11_EVENT_SET_PORT;
 *   - association is tracked with DOT11_EVENT_STA_QUERY_BSSID (the driver
 *     queues no association/deauth events in client mode).
 * One plain thread, no signals (B7). Struct layouts: docs/firmware.md (Wi-Fi).
 */
#ifndef CAMD_WPA_DRV_H
#define CAMD_WPA_DRV_H
#include "wpa.h"

typedef struct {
	int running;
	int assoc;                 /* driver reports an association */
	unsigned char bssid[6];
	int port;                  /* we opened the port (4-way done) */
	int state;                 /* WPA_IDLE / WPA_HANDSHAKE / WPA_DONE */
	wpa_stats st;
	int wrong;                 /* wpa_wrong_passphrase(&st) */
	long up_ms;                /* since wpa_drv_start */
	long assoc_ms, msg1_ms, done_ms;   /* first association / msg-1 / port open, -1 = not yet */
	long gtk_ms;               /* last GTK install */
	unsigned joins;            /* SET_RSNIE issued */
} wpa_drv_info;

/* PMK for (ssid, passphrase), cached in RAM (PBKDF2 is slow on this CPU).
 * 0 ok, -1 invalid passphrase/SSID (B5). */
int  wpa_drv_pmk(const char *ssid, const char *passphrase, unsigned char pmk[WPA_PMK_LEN]);
/* wlan0 must be up with the station MIBs. Issues SET_RSNIE (starts the
 * join) and runs the supplicant thread. 0 ok. */
int  wpa_drv_start(const unsigned char pmk[WPA_PMK_LEN]);
void wpa_drv_stop(void);                  /* before wlan0 goes down; zeroes the keys */
int  wpa_drv_rejoin(void);                /* SET_RSNIE again: restart the join */
void wpa_drv_get(wpa_drv_info *i);
void wpa_drv_forget(void);                /* drop the cached PMK (Wi-Fi disabled) */

#ifdef WPA_DRV_TEST
/* host-test harness (tests/test_wpa.c): driver requests, ctrlport, EAPOL TX
 * and the clock are stubbed; no thread, no sockets */
extern int (*wpa_drv_t_req)(void *p, size_t len);
extern int (*wpa_drv_t_ctrlport)(void);
extern int (*wpa_drv_t_send)(const unsigned char *f, size_t len);
extern long wpa_drv_t_now;
extern unsigned wpa_drv_t_logs;
int  wpa_drv_t_rng(void *ctx, unsigned char *b, size_t n);   /* provided by the test */
void wpa_drv_t_setup(const unsigned char pmk[WPA_PMK_LEN], const unsigned char mac[6]);
void wpa_drv_t_eapol(const unsigned char *pkt, size_t len);  /* Ethernet frame */
void wpa_drv_t_poll(void);                                   /* association poll */
void wpa_drv_t_timers(void);                                 /* one tick's timers */
wpa_sm *wpa_drv_t_sm(void);
int  wpa_drv_t_port(void);
int  wpa_drv_t_pmk_cached(void);
#endif
#endif
