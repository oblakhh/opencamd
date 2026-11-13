/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * netcfg — wired network config validation and safe apply/revert (see netcfg.h).
 */

#include "netcfg.h"
#include "log.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define APPLY_SCRIPT "/etc/netapply.sh"

static pthread_mutex_t g_lk = PTHREAD_MUTEX_INITIALIZER;
static wired_cfg g_try, g_prev;
static char g_host[64];
static int g_pending, g_apply_due;
static long g_deadline, g_apply_at;

static long now_s(void)                    /* monotonic: immune to NTP clock steps */
{
	struct sysinfo si;
	return sysinfo(&si) == 0 ? (long)si.uptime : 0;
}

/* Accept only canonical dotted quads (so the string can't carry anything else). */
static int ip_parse(const char *s, uint32_t *out)
{
	struct in_addr a;
	char canon[16];
	if (!s || !*s || strlen(s) > 15 || !inet_aton(s, &a))
		return 0;
	snprintf(canon, sizeof(canon), "%s", inet_ntoa(a));
	if (strcmp(canon, s) != 0)
		return 0;
	if (out)
		*out = ntohl(a.s_addr);
	return 1;
}

/* Split "a.b.c.d e.f.g.h" (space or comma) into up to two validated servers. */
static int dns_split(const char *dns, char d1[16], char d2[16])
{
	char buf[40], *tok, *save = NULL;
	int n = 0;
	strcpy(d1, "-"); strcpy(d2, "-");
	snprintf(buf, sizeof(buf), "%s", dns ? dns : "");
	for (tok = strtok_r(buf, " ,", &save); tok; tok = strtok_r(NULL, " ,", &save)) {
		if (n == 2 || !ip_parse(tok, NULL))
			return -1;
		snprintf(n ? d2 : d1, 16, "%s", tok);
		n++;
	}
	return n;
}

int netcfg_validate(const wired_cfg *w, const char **err)
{
	uint32_t ip, m, gw;
	char d1[16], d2[16];
	const char *e = NULL;

	if (strcmp(w->mode, "dhcp") == 0)
		return 0;
	if (strcmp(w->mode, "static") != 0)
		e = "mode must be dhcp or static";
	else if (!ip_parse(w->ip, &ip))
		e = "invalid IP address";
	else if (!ip_parse(w->netmask, &m) || m == 0 || m == 0xFFFFFFFFu ||
	         ((~m) & ((~m) + 1u)) != 0)
		e = "invalid netmask";
	else if ((ip & ~m) == 0 || (ip & ~m) == ~m)
		e = "IP is the network or broadcast address";
	else if (w->gateway[0] && (!ip_parse(w->gateway, &gw) ||
	         (gw & m) != (ip & m) || gw == ip || (gw & ~m) == ~m))
		e = "gateway must be a host in the same subnet";
	else if (dns_split(w->dns, d1, d2) < 0)
		e = "DNS must be up to two IPv4 addresses";
	if (err)
		*err = e;
	return e ? -1 : 0;
}

int netcfg_run(char *const argv[])
{
	int st = 0;
	pid_t p = fork();
	if (p < 0)
		return -1;
	if (p == 0) {
		execv("/bin/sh", argv);
		_exit(127);
	}
	waitpid(p, &st, 0);
	return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int run_apply(const wired_cfg *w, const char *host)
{
	char d1[16], d2[16];
	char *argv[10];
	int a = 0;

	argv[a++] = "/bin/sh";
	argv[a++] = APPLY_SCRIPT;
	if (strcmp(w->mode, "static") == 0) {
		dns_split(w->dns, d1, d2);
		argv[a++] = "static";
		argv[a++] = (char *)w->ip;
		argv[a++] = (char *)w->netmask;
		argv[a++] = (char *)(w->gateway[0] ? w->gateway : "-");
		argv[a++] = d1;
		argv[a++] = d2;
	} else {
		argv[a++] = "dhcp";
		argv[a++] = (char *)(host && *host ? host : "ic3115w");
	}
	argv[a] = NULL;
	if (netcfg_run(argv) != 0) {
		LOGW("netcfg: apply (%s) failed", w->mode);
		return -1;
	}
	LOGI("netcfg: applied %s%s%s", w->mode,
	     strcmp(w->mode, "static") == 0 ? " " : "",
	     strcmp(w->mode, "static") == 0 ? w->ip : "");
	return 0;
}

int netcfg_reapply(const wired_cfg *w, const char *hostname)
{
	return run_apply(w, hostname);
}

void netcfg_boot(const wired_cfg *committed, const char *hostname)
{
	/* rcS always starts DHCP on eth0; only a static config needs applying. */
	if (strcmp(committed->mode, "static") == 0 && netcfg_validate(committed, NULL) == 0)
		run_apply(committed, hostname);
}

int netcfg_try(const wired_cfg *w, const wired_cfg *committed, const char *hostname,
               int revert_s)
{
	if (netcfg_validate(w, NULL) != 0)
		return -1;
	pthread_mutex_lock(&g_lk);
	if (!g_pending)
		g_prev = *committed;       /* keep the original fallback across re-tries */
	g_try = *w;
	snprintf(g_host, sizeof(g_host), "%s", hostname ? hostname : "");
	g_pending = 1;
	g_apply_due = 1;
	g_apply_at = now_s() + 1;      /* let the HTTP response reach the client first */
	g_deadline = now_s() + revert_s;
	pthread_mutex_unlock(&g_lk);
	return 0;
}

int netcfg_confirm(wired_cfg *out)
{
	int had;
	pthread_mutex_lock(&g_lk);
	had = g_pending;
	if (had) {
		*out = g_try;
		g_pending = 0;
	}
	pthread_mutex_unlock(&g_lk);
	return had;
}

int netcfg_pending_left(void)
{
	long left;
	pthread_mutex_lock(&g_lk);
	left = g_pending ? g_deadline - now_s() : 0;
	pthread_mutex_unlock(&g_lk);
	return left > 0 ? (int)left : 0;
}

static void *netcfg_loop(void *arg)
{
	(void)arg;
	for (;;) {
		wired_cfg w;
		char host[64];
		int act = 0;   /* 1 = apply try, 2 = revert */
		sleep(1);
		pthread_mutex_lock(&g_lk);
		if (g_apply_due && now_s() >= g_apply_at) {
			g_apply_due = 0; w = g_try; act = 1;
		} else if (g_pending && now_s() >= g_deadline) {
			g_pending = 0; g_apply_due = 0; w = g_prev; act = 2;
		}
		snprintf(host, sizeof(host), "%s", g_host);
		pthread_mutex_unlock(&g_lk);
		if (act == 2)
			LOGW("netcfg: change not confirmed, reverting to %s", w.mode);
		if (act)
			run_apply(&w, host);
	}
	return NULL;
}

void netcfg_start(void)
{
	pthread_t t;
	if (pthread_create(&t, NULL, netcfg_loop, NULL) == 0)
		pthread_detach(t);
	else
		LOGE("netcfg: cannot start apply thread");
}
