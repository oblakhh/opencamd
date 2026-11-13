/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * wifi — Wi-Fi client (wlan0) control: validation, bring-up via
 * /etc/wifiapply.sh + the camd supplicant (wpa_drv.c), link status and scan.
 * The passphrase never leaves camd (not in any argv, environment, MIB or
 * log); only the derived keys go to the driver.
 */
#ifndef CAMD_WIFI_H
#define CAMD_WIFI_H
#include "config.h"
#include "cJSON.h"

typedef struct {
	const char *state;   /* off | standby | connecting | associated | connected | wrong_passphrase */
	char ssid[33];
	char bssid[18];
	char ip[16];
	int rssi;            /* driver signal value 0..100, -1 if unknown */
} wifi_status;

/* What a test join has shown so far (from the supplicant, wpa_drv.c). */
typedef struct {
	int assoc_seen;     /* associated at some point */
	int port_open;      /* 4-way done, port open */
	unsigned msg1;      /* EAPOL msg-1 frames received */
	int wrong;          /* wpa_wrong_passphrase(): the AP refused our msg-2 */
	unsigned msg3_ok;   /* msg-3 frames with a verified MIC (the passphrase is right) */
} wifi_obs;

enum { WT_PENDING = -1, WT_OK = 0, WT_WRONG_PASS, WT_NOT_FOUND, WT_NO_DHCP, WT_BUSY, WT_ERROR };
/* Decide a test outcome from what has been observed so far (pure). */
int  wifi_test_decide(int elapsed_ms, const wifi_obs *o);
const char *wifi_test_name(int r);
/* Synchronous test join with a locally administered MAC; wlan0 is down again
 * on return. aborted() is polled; ip gets the DHCP lease when dhcp != 0. */
int  wifi_test(const char *ssid, const char *psk, int dhcp, int (*aborted)(void),
               char ip[16], int *join_ms);

int  wifi_test_cooldown(void);   /* seconds until the AP accepts a new test */

int  wifi_validate(const wifi_cfg *w, const char **err);   /* 0 = ok */
/* up/down; mac optional. aborted (optional) is polled while waiting for the
 * port before DHCP, so an uplink change is never stalled by that wait. */
int  wifi_apply_sync(const wifi_cfg *w, const char *hostname, const char *mac, int (*aborted)(void));
/* Poll port_open() every step_ms for up to max_ms: 1 open, 0 timeout, -1 aborted(). */
int  wifi_wait_port(int max_ms, int step_ms, int (*aborted)(void), int (*port_open)(void));
void wifi_get_status(const wifi_cfg *w, wifi_status *s);
cJSON *wifi_scan(void);                                     /* array of {ssid,bssid,channel} */
#endif
