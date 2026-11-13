/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * camd — custom IP-camera daemon for the Edimax IC-3115W.
 * Single process: a V4L2 MJPEG capture thread publishes frames into a pool;
 * an embedded civetweb server fans them out as /stream.mjpg and /snapshot.jpg
 * and exposes /api endpoints. See docs/design.md.
 */
#include "log.h"
#include "config.h"
#include "config_store.h"
#include "crc32.h"
#include "auth.h"
#include "framebuf.h"
#include "capture.h"
#include "http.h"
#include "ntp.h"
#include "uploader.h"
#include "gpio.h"
#include "netcfg.h"
#include "wifi.h"
#include "net.h"

#include <pthread.h>
#include <signal.h>
#include <sys/reboot.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "version.h"

static volatile sig_atomic_t g_run = 1;
static void on_signal(int s) { (void)s; g_run = 0; }

static void usage(const char *p)
{
	fprintf(stderr,
	    "usage: %s [-p port] [-d /dev/video0] [-r /www] [-W width] [-H height]\n"
	    "          [-f fps] [-n pool_slots] [-m max_frame_kb]\n", p);
}

/* Hold the reset button ~10 s during normal operation -> wipe config and reboot
 * to defaults (admin/admin). The no-auth recovery path for a forgotten password;
 * at power-on the same button instead enters the bootloader's TFTP recovery. */
static void *reset_watch(void *arg)
{
	const char *cfgdev = arg;
	int held = 0, seen_release = 0;
	/* Startup state is logged so a wrong register read is caught before it can
	 * ever wipe: should read "up". And we only honor a hold AFTER seeing the
	 * button released once, so a stuck/misread "down" can never loop-wipe. */
	LOGI("reset-watch: started (button reads %s at startup)",
	     gpio_reset_pressed() ? "DOWN (unexpected!)" : "up");
	for (;;) {
		if (gpio_reset_pressed()) {
			if (seen_release) {
				if (held == 0)
					LOGI("reset button pressed");
				if (++held >= 20) {          /* ~10 s at 500 ms */
					LOGW("reset button held — factory reset to defaults");
					config_store_erase(cfgdev);
					sync();
					sync();
					reboot(RB_AUTOBOOT);
				}
			}
		} else {
			seen_release = 1;
			held = 0;
		}
		usleep(500000);
	}
	return 0;
}

int main(int argc, char **argv)
{
	config cfg;
	framepool pool;
	capture cap;
	struct mg_context *mg;
	http_ctx hctx;
	ntp_ctx ntp;
	uploader up;
	int port = 80, pooln = 6, maxkb = 1024, c;
	const char *device = "/dev/video0", *docroot = "/www", *cfgdev = NULL;
	int over_w = 0, over_h = 0, over_f = 0;

	log_init("camd");
	crc32("", 0); /* build the CRC table on the main thread, before any worker exists */

	/* Install signal handlers before starting threads so an early client
	 * disconnect (SIGPIPE) or a shutdown signal is always handled. */
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	while ((c = getopt(argc, argv, "p:d:r:c:W:H:f:n:m:h")) != -1) {
		switch (c) {
		case 'p': port = atoi(optarg); break;
		case 'd': device = optarg; break;
		case 'r': docroot = optarg; break;
		case 'c': cfgdev = optarg; break;
		case 'W': over_w = atoi(optarg); break;
		case 'H': over_h = atoi(optarg); break;
		case 'f': over_f = atoi(optarg); break;
		case 'n': pooln = atoi(optarg); break;
		case 'm': maxkb = atoi(optarg); break;
		case 'h': default: usage(argv[0]); return 2;
		}
	}
	auth_init();
	config_store_load(&cfg, cfgdev); /* flash config, or compiled-in defaults */
	if (cfg.system.pw_hash[0])
		auth_set_password_hash(cfg.system.pw_hash);
	auth_write_system_passwd("/var/passwd"); /* telnet shares the admin password */
	auth_apply_telnet(cfg.system.telnet);
	/* command-line overrides win over stored config (handy for testing) */
	if (over_w) cfg.video.width = over_w;
	if (over_h) cfg.video.height = over_h;
	if (over_f) cfg.video.target_fps = over_f;

	if (cfg.system.hostname[0])
		sethostname(cfg.system.hostname, strlen(cfg.system.hostname));
	LOGI("%s starting: %dx%d, device %s, http :%d, docroot %s",
	     CAMD_VERSION, cfg.video.width, cfg.video.height, device, port, docroot);

	if (framepool_init(&pool, pooln, (size_t)maxkb * 1024) != 0) {
		LOGE("framepool_init failed");
		return 1;
	}
	if (capture_start(&cap, device, &pool, cfg.video.width, cfg.video.height,
	                  cfg.video.target_fps) != 0) {
		LOGE("capture_start failed");
		framepool_destroy(&pool);
		return 1;
	}

	netcfg_start();
	net_start(&cfg.network.wired, &cfg.network.wifi, cfg.system.hostname);
	netcfg_boot(&cfg.network.wired, cfg.system.hostname);

	hctx.pool = &pool; hctx.cap = &cap; hctx.cfg = &cfg; hctx.up = &up; hctx.version = CAMD_VERSION;
	hctx.cfgdev = cfgdev;
	mg = http_start(&hctx, port, docroot);
	if (!mg) {
		capture_stop(&cap);
		framepool_destroy(&pool);
		return 1;
	}

	ntp_start(&ntp, cfg.system.ntp);   /* set the clock once the network is up */
	uploader_start(&up, &cfg.upload, &pool);
	{
		static pthread_t rt;
		if (gpio_reset_open() == 0) {
			if (pthread_create(&rt, 0, reset_watch, (void *)cfgdev) == 0)
				pthread_detach(rt);
		} else {
			LOGW("reset-watch: /dev/mem GPIO unavailable");
		}
	}

	while (g_run)
		pause();

	LOGI("camd shutting down");
	uploader_stop(&up);
	ntp_stop(&ntp);
	http_stop(mg);
	capture_stop(&cap);
	framepool_destroy(&pool);
	return 0;
}
