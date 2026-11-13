/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * Thin logging facade over syslog(3). On the device busybox syslogd captures
 * these (and forwards to a remote collector when configured). */
#ifndef CAMD_LOG_H
#define CAMD_LOG_H

#include <syslog.h>

void log_init(const char *ident);

#define LOGE(...) syslog(LOG_ERR, __VA_ARGS__)
#define LOGW(...) syslog(LOG_WARNING, __VA_ARGS__)
#define LOGI(...) syslog(LOG_INFO, __VA_ARGS__)
#define LOGD(...) syslog(LOG_DEBUG, __VA_ARGS__)

#endif
