/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * framebuf — latest-frame publish/subscribe pool (see framebuf.h).
 */

#include "framebuf.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

int framepool_init(framepool *fp, int n, size_t max_frame)
{
	if (n < 2)
		n = 2; /* need at least one pinned + one to publish into */
	memset(fp, 0, sizeof(*fp));
	fp->slots = calloc((size_t)n, sizeof(frame_slot));
	if (!fp->slots)
		return -1;
	fp->n = n;
	fp->cur = -1;
	fp->seq = 0;
	fp->max_frame = max_frame;
	pthread_mutex_init(&fp->lock, NULL);
	pthread_cond_init(&fp->cond, NULL);
	return 0;
}

void framepool_destroy(framepool *fp)
{
	int i;
	if (!fp->slots)
		return;
	for (i = 0; i < fp->n; i++)
		free(fp->slots[i].data);
	free(fp->slots);
	fp->slots = NULL;
	pthread_mutex_destroy(&fp->lock);
	pthread_cond_destroy(&fp->cond);
}

/* Pick a slot to publish into: any slot with no readers that is not the
 * current one (so readers starting "now" still see a stable latest). Prefer a
 * slot already big enough to avoid realloc. Caller holds the lock. */
static int pick_free_slot(framepool *fp, size_t need)
{
	int i, best = -1;
	for (i = 0; i < fp->n; i++) {
		if (i == fp->cur)
			continue;
		if (fp->slots[i].refs != 0)
			continue;
		if (fp->slots[i].cap >= need)
			return i; /* no realloc needed */
		if (best < 0)
			best = i;
	}
	return best;
}

int framepool_publish(framepool *fp, const unsigned char *data, size_t len)
{
	int idx, rc = 0;

	if (len > fp->max_frame) {
		pthread_mutex_lock(&fp->lock);
		fp->dropped++;
		pthread_mutex_unlock(&fp->lock);
		return -1;
	}

	pthread_mutex_lock(&fp->lock);
	idx = pick_free_slot(fp, len);
	if (idx < 0) {
		fp->dropped++;
		rc = 1; /* everything pinned; drop this frame */
		goto out;
	}
	if (fp->slots[idx].cap < len) {
		unsigned char *nb = realloc(fp->slots[idx].data, len);
		if (!nb) {
			fp->dropped++;
			rc = -1;
			goto out;
		}
		fp->slots[idx].data = nb;
		fp->slots[idx].cap = len;
	}
	memcpy(fp->slots[idx].data, data, len);
	fp->slots[idx].len = len;
	fp->slots[idx].seq = ++fp->seq;
	fp->cur = idx;
	pthread_cond_broadcast(&fp->cond);
out:
	pthread_mutex_unlock(&fp->lock);
	return rc;
}

static void deadline_from_now(struct timespec *ts, int timeout_ms)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	ts->tv_sec = tv.tv_sec + timeout_ms / 1000;
	ts->tv_nsec = (long)(tv.tv_usec * 1000) + (long)(timeout_ms % 1000) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec += 1;
		ts->tv_nsec -= 1000000000L;
	}
}

int framepool_acquire(framepool *fp, uint32_t last_seq, int timeout_ms, frame_ref *ref)
{
	int rc = 0;
	struct timespec ts;

	deadline_from_now(&ts, timeout_ms);
	pthread_mutex_lock(&fp->lock);
	while (fp->cur < 0 || fp->seq <= last_seq) {
		rc = pthread_cond_timedwait(&fp->cond, &fp->lock, &ts);
		if (rc == ETIMEDOUT)
			break;
		rc = 0; /* spurious wake or signal: re-check predicate */
	}
	if (fp->cur >= 0 && fp->seq > last_seq) {
		frame_slot *s = &fp->slots[fp->cur];
		s->refs++;
		ref->data = s->data;
		ref->len = s->len;
		ref->seq = s->seq;
		ref->slot = fp->cur;
		rc = 0;
	} else if (rc == 0) {
		rc = ETIMEDOUT; /* no newer frame and not an error */
	}
	pthread_mutex_unlock(&fp->lock);
	return rc;
}

void framepool_release(framepool *fp, frame_ref *ref)
{
	if (ref->slot < 0)
		return;
	pthread_mutex_lock(&fp->lock);
	if (fp->slots[ref->slot].refs > 0)
		fp->slots[ref->slot].refs--;
	pthread_cond_broadcast(&fp->cond); /* a slot may have freed up for the producer */
	pthread_mutex_unlock(&fp->lock);
	ref->slot = -1;
	ref->data = NULL;
}
