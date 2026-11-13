/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * Host-native tests for the frame pool. Covers latest-wins, refcount pinning,
 * oversize/all-pinned drops, blocking wait, and the producer/consumer race. */
#include "framebuf.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

static int tests, fails;
#define CHECK(cond) do { tests++; if (!(cond)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

static void test_basic_publish_acquire(void)
{
	framepool fp; frame_ref r;
	CHECK(framepool_init(&fp, 4, 1 << 20) == 0);
	CHECK(framepool_acquire(&fp, 0, 10, &r) == ETIMEDOUT); /* nothing yet */
	CHECK(framepool_publish(&fp, (const unsigned char *)"abc", 3) == 0);
	CHECK(framepool_acquire(&fp, 0, 10, &r) == 0);
	CHECK(r.len == 3 && memcmp(r.data, "abc", 3) == 0 && r.seq == 1);
	framepool_release(&fp, &r);
	/* same seq -> no newer frame -> timeout */
	CHECK(framepool_acquire(&fp, 1, 10, &r) == ETIMEDOUT);
	framepool_destroy(&fp);
}

static void test_latest_wins_and_seq(void)
{
	framepool fp; frame_ref r;
	CHECK(framepool_init(&fp, 4, 1 << 20) == 0);
	framepool_publish(&fp, (const unsigned char *)"1", 1);
	framepool_publish(&fp, (const unsigned char *)"2", 1);
	framepool_publish(&fp, (const unsigned char *)"3", 1);
	CHECK(framepool_acquire(&fp, 0, 10, &r) == 0);
	CHECK(r.seq == 3 && r.data[0] == '3'); /* newest, not a queue */
	framepool_release(&fp, &r);
	framepool_destroy(&fp);
}

static void test_oversize_dropped(void)
{
	framepool fp; frame_ref r;
	unsigned char big[64];
	CHECK(framepool_init(&fp, 4, 16) == 0);
	CHECK(framepool_publish(&fp, big, sizeof(big)) == -1); /* > max_frame */
	CHECK(fp.dropped == 1);
	CHECK(framepool_acquire(&fp, 0, 10, &r) == ETIMEDOUT); /* nothing published */
	framepool_destroy(&fp);
}

static void test_pin_blocks_reuse_but_not_producer(void)
{
	framepool fp; frame_ref a, b;
	CHECK(framepool_init(&fp, 3, 1 << 20) == 0);
	framepool_publish(&fp, (const unsigned char *)"A", 1);
	CHECK(framepool_acquire(&fp, 0, 10, &a) == 0 && a.data[0] == 'A');
	/* producer keeps going into other slots while 'a' is pinned */
	framepool_publish(&fp, (const unsigned char *)"B", 1);
	CHECK(framepool_acquire(&fp, a.seq, 10, &b) == 0 && b.data[0] == 'B');
	/* 'a' still points at the old, pinned frame */
	CHECK(a.data[0] == 'A' && a.seq == 1);
	framepool_release(&fp, &a);
	framepool_release(&fp, &b);
	framepool_destroy(&fp);
}

static void test_all_pinned_drops(void)
{
	/* 2 slots: pin both, next publish must drop (rc==1). */
	framepool fp; frame_ref a, b;
	CHECK(framepool_init(&fp, 2, 1 << 20) == 0);
	framepool_publish(&fp, (const unsigned char *)"A", 1);
	CHECK(framepool_acquire(&fp, 0, 10, &a) == 0);     /* pins slot of A (cur) */
	framepool_publish(&fp, (const unsigned char *)"B", 1); /* uses the other slot */
	CHECK(framepool_acquire(&fp, a.seq, 10, &b) == 0); /* pins slot of B (cur) */
	CHECK(framepool_publish(&fp, (const unsigned char *)"C", 1) == 1); /* both pinned -> drop */
	framepool_release(&fp, &a);
	CHECK(framepool_publish(&fp, (const unsigned char *)"C", 1) == 0); /* now a slot is free */
	framepool_release(&fp, &b);
	framepool_destroy(&fp);
}

struct prod_arg { framepool *fp; int n; };
static void *producer(void *p)
{
	struct prod_arg *pa = p;
	int i;
	for (i = 0; i < pa->n; i++) {
		unsigned char buf[4];
		memcpy(buf, &i, sizeof(i) < 4 ? sizeof(i) : 4);
		framepool_publish(pa->fp, buf, 4);
		usleep(200);
	}
	return NULL;
}

static void test_blocking_consumer(void)
{
	framepool fp; pthread_t t; struct prod_arg pa; frame_ref r;
	uint32_t last = 0; int got = 0;
	CHECK(framepool_init(&fp, 4, 1 << 20) == 0);
	pa.fp = &fp; pa.n = 20;
	pthread_create(&t, NULL, producer, &pa);
	/* consume newer-than-last frames until the producer is done */
	while (got < 20) {
		if (framepool_acquire(&fp, last, 500, &r) == 0) {
			last = r.seq; got++;
			framepool_release(&fp, &r);
		} else break; /* timeout -> producer finished */
	}
	pthread_join(t, NULL);
	CHECK(got >= 1);         /* saw at least some frames */
	CHECK(last == fp.seq);   /* caught up to the latest published */
	framepool_destroy(&fp);
}

int main(void)
{
	test_basic_publish_acquire();
	test_latest_wins_and_seq();
	test_oversize_dropped();
	test_pin_blocks_reuse_but_not_producer();
	test_all_pinned_drops();
	test_blocking_consumer();
	printf("framebuf tests: %d checks, %d failures\n", tests, fails);
	return fails ? 1 : 0;
}
