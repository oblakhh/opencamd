/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * net — uplink manager: wired preferred, Wi-Fi as cold standby (see net.h).
 */

#include "net.h"
#include "gpio.h"
#include "log.h"
#include "netcfg.h"
#include "wifi.h"

#include <net/if.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define DOWN_TICKS 10   /* 5 s without cable -> Wi-Fi */
#define UP_TICKS   20   /* 10 s of stable cable -> back to wired */

enum { U_WIRED, U_WIFI, U_NONE };

static pthread_mutex_t g_lk = PTHREAD_MUTEX_INITIALIZER;
static wired_cfg g_wired;
static wifi_cfg g_wifi;
static unsigned g_wifi_gen;
static char g_host[64], g_mac[18];
static volatile int g_uplink = U_WIRED;

/* Owner of wlan0 (see net.h). g_wlan_abort asks a running test to give it back. */
static pthread_mutex_t g_wlan = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_wlan_abort;

static void wlan_take(void)
{
	if (pthread_mutex_trylock(&g_wlan) == 0)
		return;
	g_wlan_abort = 1;           /* a test (or scan) holds wlan0: ask it to stop */
	pthread_mutex_lock(&g_wlan);
	g_wlan_abort = 0;
}

static void wlan_give(void)
{
	pthread_mutex_unlock(&g_wlan);
}

int net_wlan_test_begin(void)
{
	if (g_uplink != U_WIRED || !eth_link_up())
		return -1;
	if (pthread_mutex_trylock(&g_wlan) != 0)
		return -2;
	/* re-check under the lock: a transition may have just finished */
	if (g_uplink != U_WIRED || !eth_link_up()) {
		pthread_mutex_unlock(&g_wlan);
		return -1;
	}
	return 0;
}

int net_wlan_aborted(void)
{
	return g_wlan_abort || !eth_link_up();
}

void net_wlan_test_end(void)
{
	pthread_mutex_unlock(&g_wlan);
}

void net_wlan_lock(void)
{
	pthread_mutex_lock(&g_wlan);
}

void net_wlan_unlock(void)
{
	pthread_mutex_unlock(&g_wlan);
}

/* polled by the Wi-Fi bring-up while it waits for the port: the cable came
 * back (net.c's hand-back must not wait) or someone asked for wlan0 */
static int uplink_wait_abort(void)
{
	return g_wlan_abort || eth_link_up();
}

static void set_uplink_file(const char *ifname)
{
	FILE *f = fopen("/var/run/uplink", "w");
	if (f) { fprintf(f, "%s\n", ifname); fclose(f); }
}

static void wired_down(void)
{
	char *argv[] = { "/bin/sh", "/etc/netapply.sh", "down", NULL };
	netcfg_run(argv);
}

static void to_wifi(const wifi_cfg *w, const char *host)
{
	LOGI("net: cable gone -> switching uplink to Wi-Fi (%s, MAC %s)", w->ssid, g_mac);
	wired_down();                 /* drop the wired address so replies use wlan0 */
	set_uplink_file("wlan0");
	wifi_apply_sync(w, host, g_mac, uplink_wait_abort);
	g_uplink = U_WIFI;
}

static void to_wired(const wired_cfg *wd, const char *host)
{
	wifi_cfg off;
	LOGI("net: cable back -> switching uplink to wired (%s)", wd->mode);
	memset(&off, 0, sizeof(off));
	wifi_apply_sync(&off, host, NULL, NULL);   /* break before make: shared MAC */
	set_uplink_file("eth0");
	netcfg_reapply(wd, host);
	g_uplink = U_WIRED;
}

static void *net_loop(void *arg)
{
	int down = 0, up = 0, last = -1;
	unsigned seen_gen = 0;
	(void)arg;
	for (;;) {
		wifi_cfg w;
		wired_cfg wd;
		char host[64];
		unsigned gen;
		int link = eth_link_up();

		if (link != last) {
			unsigned v = 0;
			eth_port_status(4, &v);
			LOGI("net: wired link %s (psr4=0x%02x)", link ? "UP" : "DOWN", v);
			last = link;
		}
		down = link ? 0 : down + 1;
		up = link ? up + 1 : 0;

		pthread_mutex_lock(&g_lk);
		w = g_wifi; wd = g_wired; gen = g_wifi_gen;
		snprintf(host, sizeof(host), "%s", g_host);
		pthread_mutex_unlock(&g_lk);

		if (g_uplink == U_WIRED && down >= DOWN_TICKS && w.enabled) {
			wlan_take();
			to_wifi(&w, host);
			wlan_give();
			seen_gen = gen;
		} else if (g_uplink != U_WIRED && up >= UP_TICKS) {
			wlan_take();
			to_wired(&wd, host);
			wlan_give();
		} else if (g_uplink == U_WIFI && gen != seen_gen) {
			seen_gen = gen;
			wlan_take();
			if (w.enabled) {
				LOGI("net: Wi-Fi settings changed, re-joining");
				wifi_apply_sync(&w, host, g_mac, uplink_wait_abort);
			} else {
				wifi_cfg off;
				memset(&off, 0, sizeof(off));
				LOGI("net: Wi-Fi disabled while it was the uplink -> no uplink");
				wifi_apply_sync(&off, host, NULL, NULL);
				g_uplink = U_NONE;
			}
			wlan_give();
		} else if (g_uplink == U_NONE && w.enabled && !link && gen != seen_gen) {
			wlan_take();
			to_wifi(&w, host);
			wlan_give();
			seen_gen = gen;
		}
		memset(&w, 0, sizeof(w));
		usleep(500000);
	}
	return NULL;
}

void net_set_wifi(const wifi_cfg *wifi)
{
	pthread_mutex_lock(&g_lk);
	g_wifi = *wifi;
	g_wifi_gen++;
	pthread_mutex_unlock(&g_lk);
}

void net_set_wired(const wired_cfg *wired)
{
	pthread_mutex_lock(&g_lk);
	g_wired = *wired;
	pthread_mutex_unlock(&g_lk);
}

const char *net_uplink(void)
{
	return g_uplink == U_WIRED ? "wired" : g_uplink == U_WIFI ? "wifi" : "none";
}

void net_start(const wired_cfg *wired, const wifi_cfg *wifi, const char *hostname)
{
	pthread_t t;
	struct ifreq ifr;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	unsigned char *m;

	g_wired = *wired;
	g_wifi = *wifi;
	snprintf(g_host, sizeof(g_host), "%s", hostname && *hostname ? hostname : "ic3115w");
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "eth0");
	if (fd >= 0 && ioctl(fd, SIOCGIFHWADDR, &ifr) == 0) {
		m = (unsigned char *)ifr.ifr_hwaddr.sa_data;
		snprintf(g_mac, sizeof(g_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
		         m[0], m[1], m[2], m[3], m[4], m[5]);
	}
	if (fd >= 0)
		close(fd);
	set_uplink_file("eth0");
	if (pthread_create(&t, NULL, net_loop, NULL) == 0)
		pthread_detach(t);
}
