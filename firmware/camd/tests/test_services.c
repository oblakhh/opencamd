/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * Host test for services_syslog_target_ok() (remote syslog target validation).
 */
#include "services.h"

#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
	CHECK(services_syslog_target_ok(""));
	CHECK(services_syslog_target_ok("192.0.2.10"));
	CHECK(services_syslog_target_ok("192.0.2.10:514"));
	CHECK(services_syslog_target_ok("nas.local"));
	CHECK(services_syslog_target_ok("log-host_1.example.org:5140"));
	CHECK(services_syslog_target_ok("[2001:db8::1]:514"));
	CHECK(!services_syslog_target_ok(NULL));
	CHECK(!services_syslog_target_ok("-x"));
	CHECK(!services_syslog_target_ok("host name"));
	CHECK(!services_syslog_target_ok("host;reboot"));
	CHECK(!services_syslog_target_ok("a$(id)"));
	CHECK(!services_syslog_target_ok("0123456789012345678901234567890123456789012345678901234567890123"));
	printf("test_services: %s\n", fails ? "FAILED" : "all OK");
	return fails != 0;
}
