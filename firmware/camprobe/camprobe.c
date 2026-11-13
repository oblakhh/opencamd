/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * camprobe: enumerate and test the UVC camera (precursor of camd).
 *
 *   camprobe                      list driver, formats, frame sizes and frame intervals
 *   camprobe -s 1280x960 -f 25 -n 100 [-o /var/tmp/snap.jpg] [-q]
 *                                 capture MJPEG frames, report fps and sizes, save the last frame
 *
 * Built with the vendor toolchain against the vendor kernel headers (Linux 2.6.30, uClibc 0.9.30).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <linux/videodev2.h>

#define NBUF 4

static int fd = -1;

static int xioctl(int req, void *arg)
{
	int r;
	do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR);
	return r;
}

static const char *fourcc(unsigned int f)
{
	static char s[5];
	s[0] = f & 0xff; s[1] = (f >> 8) & 0xff; s[2] = (f >> 16) & 0xff; s[3] = (f >> 24) & 0xff; s[4] = 0;
	return s;
}

static double now(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

static void list_intervals(unsigned int pixfmt, unsigned int w, unsigned int h)
{
	struct v4l2_frmivalenum fi;
	memset(&fi, 0, sizeof(fi));
	fi.pixel_format = pixfmt; fi.width = w; fi.height = h;
	printf("      intervals:");
	for (fi.index = 0; xioctl(VIDIOC_ENUM_FRAMEINTERVALS, &fi) == 0; fi.index++) {
		if (fi.type == V4L2_FRMIVAL_TYPE_DISCRETE)
			printf(" %u/%u (%.1f fps)", fi.discrete.numerator, fi.discrete.denominator,
			       (double)fi.discrete.denominator / fi.discrete.numerator);
		else {
			printf(" range %u/%u..%u/%u", fi.stepwise.min.numerator, fi.stepwise.min.denominator,
			       fi.stepwise.max.numerator, fi.stepwise.max.denominator);
			break;
		}
	}
	printf("\n");
}

static void list_all(void)
{
	struct v4l2_capability cap;
	struct v4l2_fmtdesc fd_;
	struct v4l2_frmsizeenum fs;

	if (xioctl(VIDIOC_QUERYCAP, &cap) == 0)
		printf("driver=%s card=%s bus=%s version=0x%x caps=0x%08x\n",
		       cap.driver, cap.card, cap.bus_info, cap.version, cap.capabilities);
	memset(&fd_, 0, sizeof(fd_));
	fd_.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	for (fd_.index = 0; xioctl(VIDIOC_ENUM_FMT, &fd_) == 0; fd_.index++) {
		printf("format %u: %s '%s'%s\n", fd_.index, fourcc(fd_.pixelformat), fd_.description,
		       (fd_.flags & V4L2_FMT_FLAG_COMPRESSED) ? " (compressed)" : "");
		memset(&fs, 0, sizeof(fs));
		fs.pixel_format = fd_.pixelformat;
		for (fs.index = 0; xioctl(VIDIOC_ENUM_FRAMESIZES, &fs) == 0; fs.index++) {
			if (fs.type != V4L2_FRMSIZE_TYPE_DISCRETE) {
				printf("    sizes: stepwise %ux%u..%ux%u\n", fs.stepwise.min_width, fs.stepwise.min_height,
				       fs.stepwise.max_width, fs.stepwise.max_height);
				break;
			}
			printf("    %ux%u\n", fs.discrete.width, fs.discrete.height);
			list_intervals(fs.pixel_format, fs.discrete.width, fs.discrete.height);
		}
	}
}

static int capture(unsigned int w, unsigned int h, unsigned int fps, int frames, const char *out, int quiet)
{
	struct v4l2_format fmt;
	struct v4l2_streamparm parm;
	struct v4l2_requestbuffers req;
	struct v4l2_buffer buf;
	void *mem[NBUF];
	size_t len[NBUF];
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	unsigned long total = 0, minsz = ~0UL, maxsz = 0;
	double t0 = 0, t1;
	int i, n;

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = type;
	fmt.fmt.pix.width = w; fmt.fmt.pix.height = h;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
	fmt.fmt.pix.field = V4L2_FIELD_ANY;
	if (xioctl(VIDIOC_S_FMT, &fmt) < 0) { perror("VIDIOC_S_FMT"); return 1; }
	memset(&parm, 0, sizeof(parm));
	parm.type = type;
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = fps;
	if (xioctl(VIDIOC_S_PARM, &parm) < 0) perror("VIDIOC_S_PARM");
	xioctl(VIDIOC_G_PARM, &parm);
	if (!quiet)
		printf("format %ux%u %s, sizeimage=%u, timeperframe=%u/%u\n", fmt.fmt.pix.width, fmt.fmt.pix.height,
		       fourcc(fmt.fmt.pix.pixelformat), fmt.fmt.pix.sizeimage,
		       parm.parm.capture.timeperframe.numerator, parm.parm.capture.timeperframe.denominator);

	memset(&req, 0, sizeof(req));
	req.count = NBUF; req.type = type; req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(VIDIOC_REQBUFS, &req) < 0 || req.count < 2) { perror("VIDIOC_REQBUFS"); return 1; }
	for (i = 0; i < (int)req.count; i++) {
		memset(&buf, 0, sizeof(buf));
		buf.type = type; buf.memory = V4L2_MEMORY_MMAP; buf.index = i;
		if (xioctl(VIDIOC_QUERYBUF, &buf) < 0) { perror("VIDIOC_QUERYBUF"); return 1; }
		len[i] = buf.length;
		mem[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
		if (mem[i] == MAP_FAILED) { perror("mmap"); return 1; }
		if (xioctl(VIDIOC_QBUF, &buf) < 0) { perror("VIDIOC_QBUF"); return 1; }
	}
	if (xioctl(VIDIOC_STREAMON, &type) < 0) { perror("VIDIOC_STREAMON"); return 1; }

	for (n = 0; n < frames; n++) {
		memset(&buf, 0, sizeof(buf));
		buf.type = type; buf.memory = V4L2_MEMORY_MMAP;
		if (xioctl(VIDIOC_DQBUF, &buf) < 0) { perror("VIDIOC_DQBUF"); return 1; }
		if (n == 0)
			t0 = now(); /* the first frame includes sensor start-up; time from here */
		else {
			total += buf.bytesused;
			if (buf.bytesused < minsz) minsz = buf.bytesused;
			if (buf.bytesused > maxsz) maxsz = buf.bytesused;
		}
		if (n == frames - 1 && out) {
			FILE *f = fopen(out, "wb");
			if (!f || fwrite(mem[buf.index], 1, buf.bytesused, f) != buf.bytesused) perror(out);
			if (f) fclose(f);
		}
		if (xioctl(VIDIOC_QBUF, &buf) < 0) { perror("VIDIOC_QBUF"); return 1; }
	}
	t1 = now();
	xioctl(VIDIOC_STREAMOFF, &type);
	for (i = 0; i < (int)req.count; i++)
		munmap(mem[i], len[i]);
	if (!quiet && frames > 1)
		printf("%d frames in %.2f s = %.2f fps; frame size avg %lu min %lu max %lu bytes\n", frames - 1, t1 - t0,
		       (frames - 1) / (t1 - t0), total / (frames - 1), minsz, maxsz);
	return 0;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/video0", *out = NULL;
	unsigned int w = 0, h = 0, fps = 30;
	int frames = 0, quiet = 0, c, r;

	while ((c = getopt(argc, argv, "d:s:f:n:o:q")) != -1) {
		switch (c) {
		case 'd': dev = optarg; break;
		case 's': if (sscanf(optarg, "%ux%u", &w, &h) != 2) goto usage; break;
		case 'f': fps = atoi(optarg); break;
		case 'n': frames = atoi(optarg); break;
		case 'o': out = optarg; break;
		case 'q': quiet = 1; break;
		default: goto usage;
		}
	}
	fd = open(dev, O_RDWR);
	if (fd < 0) { perror(dev); return 1; }
	if (!w) {
		list_all();
		r = 0;
	} else {
		r = capture(w, h, fps ? fps : 30, frames > 0 ? frames : 1, out, quiet);
	}
	close(fd);
	return r;
usage:
	fprintf(stderr, "usage: %s [-d dev] [-s WxH [-f fps] [-n frames] [-o file.jpg] [-q]]\n", argv[0]);
	return 2;
}
