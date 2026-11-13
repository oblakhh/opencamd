/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * capture — V4L2 MJPEG capture thread feeding the frame pool (see capture.h).
 */

#include "capture.h"
#include "log.h"
#include "mjpeg.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <linux/videodev2.h>

#define NBUF 4            /* V4L2 mmap buffers (agreed default) */
#define STALL_MS 3000     /* no frame for this long -> reopen */

struct vbuf { void *start; size_t length; };

static int xioctl(int fd, int req, void *arg)
{
	int r;
	do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
	return r;
}

/* Open + configure + STREAMON. Returns fd and fills bufs[NBUF], or -1. */
static int dev_open(capture *cap, struct vbuf *bufs)
{
	struct v4l2_capability vc;
	struct v4l2_format fmt;
	struct v4l2_streamparm parm;
	struct v4l2_requestbuffers req;
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	int fd, i;

	fd = open(cap->device, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		LOGE("capture: open %s: %s", cap->device, strerror(errno));
		return -1;
	}
	if (xioctl(fd, VIDIOC_QUERYCAP, &vc) < 0 ||
	    !(vc.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
	    !(vc.capabilities & V4L2_CAP_STREAMING)) {
		LOGE("capture: %s is not a streaming capture device", cap->device);
		goto fail;
	}

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = type;
	fmt.fmt.pix.width = cap->width;
	fmt.fmt.pix.height = cap->height;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
	fmt.fmt.pix.field = V4L2_FIELD_ANY;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		LOGE("capture: S_FMT %dx%d MJPEG: %s", cap->width, cap->height, strerror(errno));
		goto fail;
	}
	if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
		LOGE("capture: device refused MJPEG");
		goto fail;
	}
	cap->width = fmt.fmt.pix.width;
	cap->height = fmt.fmt.pix.height;

	memset(&parm, 0, sizeof(parm));
	parm.type = type;
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = cap->fps > 0 ? cap->fps : 10;
	if (xioctl(fd, VIDIOC_S_PARM, &parm) < 0)
		LOGW("capture: S_PARM fps: %s (continuing)", strerror(errno));

	memset(&req, 0, sizeof(req));
	req.count = NBUF;
	req.type = type;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
		LOGE("capture: REQBUFS: %s", strerror(errno));
		goto fail;
	}
	for (i = 0; i < (int)req.count; i++) {
		struct v4l2_buffer b;
		memset(&b, 0, sizeof(b));
		b.type = type; b.memory = V4L2_MEMORY_MMAP; b.index = i;
		if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) { LOGE("capture: QUERYBUF"); goto fail; }
		bufs[i].length = b.length;
		bufs[i].start = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
		if (bufs[i].start == MAP_FAILED) { LOGE("capture: mmap"); goto fail; }
		if (xioctl(fd, VIDIOC_QBUF, &b) < 0) { LOGE("capture: QBUF"); goto fail; }
	}
	for (; i < NBUF; i++) { bufs[i].start = NULL; bufs[i].length = 0; }

	if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
		LOGE("capture: STREAMON: %s", strerror(errno));
		goto fail;
	}
	LOGI("capture: streaming %dx%d MJPEG on %s", cap->width, cap->height, cap->device);
	return fd;
fail:
	for (i = 0; i < NBUF; i++)
		if (bufs[i].start && bufs[i].start != MAP_FAILED)
			munmap(bufs[i].start, bufs[i].length);
	close(fd);
	return -1;
}

static void dev_close(int fd, struct vbuf *bufs)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	int i;
	if (fd < 0)
		return;
	xioctl(fd, VIDIOC_STREAMOFF, &type);
	for (i = 0; i < NBUF; i++)
		if (bufs[i].start && bufs[i].start != MAP_FAILED)
			munmap(bufs[i].start, bufs[i].length);
	close(fd);
}

static long now_ms(void)
{
	/* Monotonic: the wall clock jumps from 1970 to the present when NTP syncs, which
	 * made intervals across the jump meaningless (and tv_sec * 1000 overflowed). */
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)((unsigned long)ts.tv_sec * 1000UL + (unsigned long)(ts.tv_nsec / 1000000L));
}

static void *capture_loop(void *arg)
{
	capture *cap = arg;
	struct vbuf bufs[NBUF];
	int fd = -1;
	int backoff = 200;
	long last_frame = 0, fps_t0 = 0;
	unsigned long fps_n = 0;

	while (cap->running) {
		struct pollfd pfd;
		struct v4l2_buffer b;
		int pr;

		if (fd < 0) {
			memset(bufs, 0, sizeof(bufs));
			fd = dev_open(cap, bufs);
			if (fd < 0) {
				cap->alive = 0;
				usleep(backoff * 1000);
				if (backoff < 5000) backoff *= 2;
				continue;
			}
			backoff = 200;
			last_frame = now_ms();
			fps_t0 = last_frame;
			fps_n = 0;
			cap->alive = 1;
		}

		pfd.fd = fd; pfd.events = POLLIN;
		pr = poll(&pfd, 1, 500);
		if (pr < 0) {
			if (errno == EINTR) continue;
			LOGW("capture: poll: %s", strerror(errno));
			dev_close(fd, bufs); fd = -1; continue;
		}
		if (pr == 0) {
			if (now_ms() - last_frame > STALL_MS) {
				LOGW("capture: stalled, reopening %s", cap->device);
				dev_close(fd, bufs); fd = -1; cap->alive = 0;
			}
			continue;
		}

		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b.memory = V4L2_MEMORY_MMAP;
		if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) {
			if (errno == EAGAIN) continue;
			LOGW("capture: DQBUF: %s", strerror(errno));
			dev_close(fd, bufs); fd = -1; continue;
		}
		if (b.index < NBUF && b.bytesused > 0) {
			/* Normalize to a self-contained JPEG (insert the standard DHT if the
			 * sensor omitted it) so strict decoders like Safari can render it. */
			size_t fixed = cap->fixbuf
			    ? mjpeg_ensure_dht(bufs[b.index].start, b.bytesused, cap->fixbuf, cap->fixcap)
			    : 0;
			if (fixed)
				framepool_publish(cap->pool, cap->fixbuf, fixed);
			else
				framepool_publish(cap->pool, bufs[b.index].start, b.bytesused);
		}
		xioctl(fd, VIDIOC_QBUF, &b);

		cap->frames++;
		last_frame = now_ms();
		if (++fps_n >= 15) { /* update rolling fps every ~15 frames */
			long dt = last_frame - fps_t0;
			if (dt > 0) cap->fps_measured = (float)fps_n * 1000.0f / (float)dt;
			fps_t0 = last_frame; fps_n = 0;
		}
	}

	dev_close(fd, bufs);
	cap->alive = 0;
	return NULL;
}

int capture_start(capture *cap, const char *device, framepool *pool,
                  int width, int height, int fps)
{
	memset(cap, 0, sizeof(*cap));
	snprintf(cap->device, sizeof(cap->device), "%s", device);
	cap->width = width; cap->height = height; cap->fps = fps;
	cap->pool = pool;
	cap->fixcap = 1024 * 1024;   /* ample for a 1280x1024 MJPEG frame + DHT */
	cap->fixbuf = malloc(cap->fixcap);
	if (!cap->fixbuf) cap->fixcap = 0;   /* non-fatal: publish raw frames */
	cap->running = 1;
	if (pthread_create(&cap->thread, NULL, capture_loop, cap) != 0) {
		LOGE("capture: pthread_create failed");
		cap->running = 0;
		free(cap->fixbuf); cap->fixbuf = NULL; cap->fixcap = 0;
		return -1;
	}
	return 0;
}

void capture_stop(capture *cap)
{
	if (cap->running) {
		cap->running = 0;
		pthread_join(cap->thread, NULL);
	}
	free(cap->fixbuf);
	cap->fixbuf = NULL;
	cap->fixcap = 0;
}

int capture_reconfigure(capture *cap, int width, int height, int fps)
{
	char device[64];
	framepool *pool = cap->pool;
	snprintf(device, sizeof(device), "%s", cap->device);
	LOGI("capture: reconfigure to %dx%d @%d", width, height, fps);
	capture_stop(cap);
	return capture_start(cap, device, pool, width, height, fps);
}
