/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * services — telnetd and syslogd follow the settings (see services.h).
 */

#include "services.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Run a busybox tool without inheriting camd's descriptors (HTTP socket, video
 * device, flash): telnetd and syslogd daemonize and must not keep those open
 * across a camd restart. Waits for the direct child only. */
static void run_clean(const char *path, char *const argv[])
{
	pid_t p = fork();
	if (p == 0) {
		int fd;
		for (fd = 3; fd < 1024; fd++)
			close(fd);
		execv(path, argv);
		_exit(127);
	}
	if (p > 0)
		waitpid(p, NULL, 0);
}

void services_apply_telnet(int on)
{
	static int state = -1;   /* unknown at start: rcS does not start telnetd */
	char *start[] = { "telnetd", NULL };
	char *stop[] = { "killall", "telnetd", NULL };
	on = on ? 1 : 0;
	if (on == state)
		return;
	if (on)
		run_clean("/bin/telnetd", start);
	else
		run_clean("/bin/killall", stop);
	state = on;
}

int services_syslog_target_ok(const char *r)
{
	size_t i, n;
	if (!r)
		return 0;
	n = strlen(r);
	if (n == 0)
		return 1;
	if (n > 63 || r[0] == '-')
		return 0;
	for (i = 0; i < n; i++) {
		char c = r[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		      c == '.' || c == '-' || c == '_' || c == ':' || c == '[' || c == ']'))
			return 0;
	}
	return 1;
}

void services_apply_syslog(const char *remote, int level)
{
	/* rcS starts "syslogd -L -O /var/log/messages -s 200": local only, all levels. */
	static char cur_remote[64] = "";
	static int cur_level = 7;
	char lvl[4];
	char *stop[] = { "killall", "syslogd", NULL };
	char *argv[12];
	int a = 0;

	if (!remote || !services_syslog_target_ok(remote))
		remote = "";
	if (level < 0 || level > 7)
		level = 7;
	if (strcmp(remote, cur_remote) == 0 && level == cur_level)
		return;

	run_clean("/bin/killall", stop);
	usleep(200 * 1000);           /* let the old instance release /dev/log */
	snprintf(lvl, sizeof(lvl), "%d", level + 1);   /* busybox -l N: priority < N */
	argv[a++] = "syslogd";
	argv[a++] = "-O"; argv[a++] = "/var/log/messages";
	argv[a++] = "-s"; argv[a++] = "200";
	argv[a++] = "-l"; argv[a++] = lvl;
	if (remote[0]) {
		argv[a++] = "-R"; argv[a++] = (char *)remote;
		argv[a++] = "-L";            /* keep the local log too */
	}
	argv[a] = NULL;
	run_clean("/bin/syslogd", argv);

	snprintf(cur_remote, sizeof(cur_remote), "%s", remote);
	cur_level = level;
	LOGI("syslog: %s%s, level %d", remote[0] ? "local + remote " : "local only",
	     remote, level);
}
