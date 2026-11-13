/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * ntp — a minimal SNTP client. Sets the system clock from the configured NTP
 * server on boot (retrying until the network/DNS are up) and re-syncs hourly.
 * The device has no RTC, so without this the clock sits at 1970 and every log
 * line and status timestamp is meaningless.
 */
#ifndef CAMD_NTP_H
#define CAMD_NTP_H

#include <pthread.h>

typedef struct {
	pthread_t thread;
	volatile int running;
	const char *server;   /* points at cfg.system.ntp; read live each cycle so a
	                       * config change takes effect without a restart */
} ntp_ctx;

/* One synchronous query; sets the clock on success. Returns 0 on success. */
int ntp_sync_once(const char *server);

/* Background sync thread: retry every 60s until the first success, then hourly.
 * `server` must point at storage that outlives the thread (e.g. cfg.system.ntp). */
int ntp_start(ntp_ctx *n, const char *server);
void ntp_stop(ntp_ctx *n);

#endif
