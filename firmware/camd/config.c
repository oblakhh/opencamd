/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * config — compiled-in configuration defaults (see config.h).
 */

#include "config.h"
#include <string.h>

void config_defaults(config *c)
{
	memset(c, 0, sizeof(*c));
	c->video.width = 1280;
	c->video.height = 960;
	c->video.quality = 80;
	c->video.target_fps = 10; /* module ceiling; see docs/hardware.md */
	strcpy(c->system.hostname, "ic3115w");
	strcpy(c->system.ntp, "pool.ntp.org");
	c->system.syslog_remote[0] = '\0';
	c->system.syslog_level = 6; /* LOG_INFO */
	c->system.telnet = 0;   /* unencrypted; enable in Settings > System when needed */

	c->upload.enabled = 0;
	c->upload.url[0] = '\0';
	c->upload.interval = 10;
	strcpy(c->upload.method, "POST");
	strcpy(c->upload.field, "file");
	c->upload.headers[0] = '\0';

	strcpy(c->network.wired.mode, "dhcp");
}
