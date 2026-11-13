/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * Host unit tests for netcfg_validate (wired config validation).
 */

#include "netcfg.h"
#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static int v(const char *mode, const char *ip, const char *mask, const char *gw, const char *dns)
{
	wired_cfg w;
	memset(&w, 0, sizeof(w));
	snprintf(w.mode, sizeof(w.mode), "%s", mode);
	snprintf(w.ip, sizeof(w.ip), "%s", ip);
	snprintf(w.netmask, sizeof(w.netmask), "%s", mask);
	snprintf(w.gateway, sizeof(w.gateway), "%s", gw);
	snprintf(w.dns, sizeof(w.dns), "%s", dns);
	return netcfg_validate(&w, NULL);
}

int main(void)
{
	CHECK(v("dhcp", "", "", "", "") == 0);
	CHECK(v("static", "192.0.2.65", "255.255.255.0", "192.0.2.1", "192.0.2.1") == 0);
	CHECK(v("static", "192.0.2.65", "255.255.255.0", "", "") == 0);             /* no gw/dns */
	CHECK(v("static", "192.0.2.65", "255.255.255.0", "192.0.2.1", "1.1.1.1,8.8.8.8") == 0);
	CHECK(v("bogus", "192.0.2.65", "255.255.255.0", "", "") != 0);
	CHECK(v("static", "192.0.2", "255.255.255.0", "", "") != 0);                /* short form */
	CHECK(v("static", "192.0.2.65;reboot", "255.255.255.0", "", "") != 0);     /* injection */
	CHECK(v("static", "192.0.2.065", "255.255.255.0", "", "") != 0);           /* non-canonical */
	CHECK(v("static", "192.0.2.65", "255.0.255.0", "", "") != 0);              /* non-contiguous */
	CHECK(v("static", "192.0.2.65", "255.255.255.255", "", "") != 0);          /* /32 */
	CHECK(v("static", "192.0.2.0", "255.255.255.0", "", "") != 0);             /* network addr */
	CHECK(v("static", "192.0.2.255", "255.255.255.0", "", "") != 0);           /* broadcast */
	CHECK(v("static", "192.0.2.65", "255.255.255.0", "192.0.3.1", "") != 0);  /* gw off-subnet */
	CHECK(v("static", "192.0.2.65", "255.255.255.0", "192.0.2.65", "") != 0); /* gw == ip */
	CHECK(v("static", "192.0.2.65", "255.255.255.0", "", "1.1.1.1 2.2.2.2 3.3.3.3") != 0);
	CHECK(v("static", "192.0.2.65", "255.255.255.0", "", "dns.example") != 0);
	printf("netcfg tests: %d checks, %d failures\n", checks, fails);
	return fails != 0;
}
