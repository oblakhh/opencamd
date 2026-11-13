/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * net — uplink manager. Wired is preferred; Wi-Fi is a cold standby that takes
 * over with the eth0 MAC (one identity on the LAN) when the cable is gone, and
 * hands back break-before-make when it returns. Link comes from the switch
 * port status (gpio.c) because eth0's carrier never changes on this driver.
 */
#ifndef CAMD_NET_H
#define CAMD_NET_H
#include "config.h"

void net_start(const wired_cfg *wired, const wifi_cfg *wifi, const char *hostname);
void net_set_wifi(const wifi_cfg *wifi);     /* new Wi-Fi config (from the API) */
void net_set_wired(const wired_cfg *wired);  /* newly committed wired config */
const char *net_uplink(void);                /* "wired" | "wifi" | "none" */

/* wlan0 ownership. net.c owns wlan0; a Wi-Fi test (POST /api/wifi/test) or a
 * scan borrows it. net.c takes the same lock for every uplink transition and,
 * when it has to, raises the abort flag so a running test bails out quickly
 * (wlan0 down, lock released) and failover proceeds. */
int  net_wlan_test_begin(void);   /* 0 = ok; -1 = cable is not the uplink; -2 = busy */
int  net_wlan_aborted(void);      /* polled by the test: net.c needs wlan0 now */
void net_wlan_test_end(void);
void net_wlan_lock(void);         /* blocking (scan) */
void net_wlan_unlock(void);
#endif
