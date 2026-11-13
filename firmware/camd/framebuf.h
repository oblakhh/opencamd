/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * framebuf — latest-frame publish/subscribe pool for MJPEG fan-out.
 *
 * One producer (the capture thread) publishes complete JPEG frames; many
 * consumers (HTTP stream/snapshot handlers) read the most recent frame. Slow
 * consumers never stall the producer: a reader pins the slot it is sending
 * (refcount), and the producer only ever reuses a slot with refcount 0, so at
 * worst a new frame is dropped when every slot is pinned — exactly the right
 * behaviour for live MJPEG.
 */
#ifndef CAMD_FRAMEBUF_H
#define CAMD_FRAMEBUF_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

typedef struct {
	unsigned char *data;   /* owned buffer, grown on demand */
	size_t cap;            /* allocated capacity */
	size_t len;            /* valid bytes of the current frame */
	uint32_t seq;          /* sequence of the frame currently in this slot */
	int refs;              /* readers currently holding this slot */
} frame_slot;

typedef struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;   /* signalled on each publish and each release */
	frame_slot *slots;
	int n;                 /* number of slots */
	int cur;               /* index of the latest published slot, or -1 */
	uint32_t seq;          /* sequence of the last published frame (0 = none) */
	size_t max_frame;      /* hard cap on a single frame's size */
	unsigned long dropped; /* frames dropped because all slots were pinned */
} framepool;

/* A borrowed reference to the latest frame; valid until framepool_release(). */
typedef struct {
	const unsigned char *data;
	size_t len;
	uint32_t seq;
	int slot;              /* internal: index to release */
} frame_ref;

/* Create a pool of n slots, each frame capped at max_frame bytes. Returns 0 on success. */
int framepool_init(framepool *fp, int n, size_t max_frame);
void framepool_destroy(framepool *fp);

/* Producer: publish a complete frame (copied in). Returns 0 on success,
 * -1 on oversize (dropped), 1 if all slots were pinned (dropped). */
int framepool_publish(framepool *fp, const unsigned char *data, size_t len);

/* Consumer: wait until a frame newer than last_seq is available (or timeout),
 * then borrow it. Pass last_seq = 0 to get the current frame immediately.
 * Returns 0 and fills ref on success; ETIMEDOUT on timeout; other errno on error.
 * On success the caller MUST call framepool_release(fp, &ref). */
int framepool_acquire(framepool *fp, uint32_t last_seq, int timeout_ms, frame_ref *ref);

/* Release a reference obtained from framepool_acquire. */
void framepool_release(framepool *fp, frame_ref *ref);

#endif /* CAMD_FRAMEBUF_H */
