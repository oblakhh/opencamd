/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * capture — V4L2 MJPEG capture thread.
 *
 * Opens /dev/video0, negotiates MJPEG at the requested size, streams via mmap,
 * and publishes each complete JPEG frame into a framepool. Self-heals: on I/O
 * errors or a stall it closes and reopens the device with backoff.
 */
#ifndef CAMD_CAPTURE_H
#define CAMD_CAPTURE_H

#include <pthread.h>
#include <stddef.h>
#include "framebuf.h"

typedef struct {
	char device[64];
	int width, height, fps;
	framepool *pool;

	pthread_t thread;
	volatile int running;

	unsigned char *fixbuf;   /* scratch for DHT-normalized JPEG (see mjpeg.c) */
	size_t fixcap;

	/* stats (read without lock; monotonic/last-wins, fine for a status page) */
	volatile unsigned long frames;  /* total frames published */
	volatile float fps_measured;    /* rolling measured fps */
	volatile int alive;             /* 1 while the device is streaming */
} capture;

/* Start the capture thread. Returns 0 on success. */
int capture_start(capture *cap, const char *device, framepool *pool,
                  int width, int height, int fps);

/* Stop and join the capture thread. */
void capture_stop(capture *cap);

/* Restart capture with new parameters (e.g. after a resolution change). */
int capture_reconfigure(capture *cap, int width, int height, int fps);

#endif
