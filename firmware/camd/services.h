/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * services — keep the busybox services camd owns (telnetd, syslogd) in line with
 * the settings. Applied at startup and after every settings save.
 */
#ifndef CAMD_SERVICES_H
#define CAMD_SERVICES_H

/* Start or stop the telnet shell (system.telnet). */
void services_apply_telnet(int on);

/* (Re)start syslogd for the remote target (system.syslog_remote, "" = local only,
 * else host or host:port, UDP 514 by default) and the level (system.syslog_level,
 * 0..7: messages up to and including that priority are logged). No-op if
 * nothing changed. */
void services_apply_syslog(const char *remote, int level);

/* Pure validation (host-tested): "" or host[:port] / [v6]:port of safe
 * characters, not starting with '-'. Returns 1 if acceptable. */
int services_syslog_target_ok(const char *remote);

#endif
