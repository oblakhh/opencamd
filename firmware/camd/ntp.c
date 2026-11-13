/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * ntp — minimal SNTP client that sets the system clock (see ntp.h).
 */

#include "ntp.h"
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

/* Seconds between the NTP epoch (1900) and the Unix epoch (1970). */
#define NTP_EPOCH 2208988800UL

int ntp_sync_once(const char *server)
{
	struct addrinfo hints, *res = NULL, *ai;
	int rc = -1;
	unsigned char pkt[48];

	if (!server || !server[0])
		return -1;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	if (getaddrinfo(server, "123", &hints, &res) != 0 || !res) {
		LOGW("ntp: cannot resolve %s", server);
		return -1;
	}
	for (ai = res; ai; ai = ai->ai_next) {
		struct timeval tv;
		fd_set rf;
		int s = socket(ai->ai_family, SOCK_DGRAM, 0);
		if (s < 0)
			continue;
		memset(pkt, 0, sizeof(pkt));
		pkt[0] = 0x1B;                       /* LI=0, VN=3, Mode=3 (client) */
		if (sendto(s, pkt, sizeof(pkt), 0, ai->ai_addr, ai->ai_addrlen)
		    != (ssize_t)sizeof(pkt)) {
			close(s);
			continue;
		}
		tv.tv_sec = 5; tv.tv_usec = 0;
		FD_ZERO(&rf); FD_SET(s, &rf);
		if (select(s + 1, &rf, NULL, NULL, &tv) > 0 &&
		    recv(s, pkt, sizeof(pkt), 0) == (ssize_t)sizeof(pkt)) {
			/* Transmit timestamp (seconds) at bytes 40..43, big-endian. */
			unsigned long secs = ((unsigned long)pkt[40] << 24) |
			                     ((unsigned long)pkt[41] << 16) |
			                     ((unsigned long)pkt[42] << 8) |
			                     (unsigned long)pkt[43];
			if (secs > NTP_EPOCH) {
				struct timeval now;
				now.tv_sec = (time_t)(secs - NTP_EPOCH);
				now.tv_usec = 0;
				if (settimeofday(&now, NULL) == 0) {
					LOGI("ntp: clock set from %s", server);
					rc = 0;
				} else {
					LOGW("ntp: settimeofday: %s", strerror(errno));
				}
			}
		}
		close(s);
		if (rc == 0)
			break;
	}
	freeaddrinfo(res);
	return rc;
}

static void *ntp_loop(void *arg)
{
	ntp_ctx *n = arg;
	int synced = 0;
	while (n->running) {
		int wait;
		if (ntp_sync_once(n->server) == 0)
			synced = 1;
		/* Retry every 60s until the first sync (network/DNS may still be
		 * coming up); once set, re-sync hourly to correct drift. */
		wait = synced ? 3600 : 60;
		while (wait-- > 0 && n->running)
			sleep(1);
	}
	return NULL;
}

int ntp_start(ntp_ctx *n, const char *server)
{
	memset(n, 0, sizeof(*n));
	n->server = server;          /* live pointer into the config (read each cycle) */
	n->running = 1;
	if (pthread_create(&n->thread, NULL, ntp_loop, n) != 0) {
		LOGE("ntp: pthread_create failed");
		n->running = 0;
		return -1;
	}
	return 0;
}

void ntp_stop(ntp_ctx *n)
{
	if (!n->running)
		return;
	n->running = 0;
	pthread_join(n->thread, NULL);
}
