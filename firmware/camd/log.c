/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * log — syslog(3) setup (see log.h).
 */

#include "log.h"

void log_init(const char *ident)
{
	openlog(ident, LOG_PID, LOG_DAEMON);
}
