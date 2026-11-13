/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * HTTP layer: civetweb handlers for the live stream, snapshot, status, the
 * static UI and the control-plane /api endpoints (config, auth, network, OTA).
 */
#ifndef CAMD_HTTP_H
#define CAMD_HTTP_H

#include "framebuf.h"
#include "capture.h"
#include "config.h"
#include "uploader.h"

typedef struct {
	framepool *pool;
	capture *cap;
	config *cfg;
	uploader *up;
	const char *version;
	const char *cfgdev;   /* MTD device for config persistence (NULL = /dev/mtd0) */
} http_ctx;

struct mg_context;

/* Start the embedded server on the given port, serving static files from
 * docroot and the camd handlers. Returns the civetweb context, or NULL. */
struct mg_context *http_start(http_ctx *ctx, int port, const char *docroot);
void http_stop(struct mg_context *mg);

#endif
