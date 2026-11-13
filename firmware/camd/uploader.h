/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * uploader — push the current JPEG to a configured HTTP(S) endpoint on an
 * interval (PrusaLink/OctoPrint-style; HTTPS via mbedTLS with HAVE_TLS).
 * Reads config live so changes apply without a restart; exposes last-result
 * status for /api/status.
 */
#ifndef CAMD_UPLOADER_H
#define CAMD_UPLOADER_H

#include <pthread.h>
#include "config.h"
#include "framebuf.h"

typedef struct {
	pthread_t thread;
	volatile int running;
	const upload_cfg *cfg;         /* live pointer into the running config */
	framepool *pool;

	volatile int last_code;        /* last HTTP status (0 = no/failed connect) */
	volatile int ok;               /* last attempt returned 2xx */
	volatile unsigned long count;  /* successful uploads since boot */
} uploader;

int uploader_start(uploader *u, const upload_cfg *cfg, framepool *pool);
void uploader_stop(uploader *u);

#endif
