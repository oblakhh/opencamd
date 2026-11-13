/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * Host unit tests for wifi_validate and the Wi-Fi test result decisions.
 */

#include "wifi.h"
#include <stdio.h>
#include <string.h>
static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
static int v(int en, const char *ssid, const char *pw)
{
	wifi_cfg w; memset(&w, 0, sizeof(w)); w.enabled = en;
	snprintf(w.ssid, sizeof(w.ssid), "%s", ssid); snprintf(w.passphrase, sizeof(w.passphrase), "%s", pw);
	return wifi_validate(&w, NULL);
}
static int polls, abort_after;
static int never_open(void) { polls++; return 0; }
static int open_third(void) { return ++polls >= 3; }
static int abort_soon(void) { return polls >= abort_after; }

int main(void)
{
	CHECK(v(0, "", "") == 0);                       /* disabled: anything empty is fine */
	CHECK(v(1, "example-ssid", "12345678") == 0);
	CHECK(v(1, "My Net 5", "with spaces & symbols!") == 0);
	CHECK(v(1, "", "12345678") != 0);               /* empty ssid */
	CHECK(v(1, "example-ssid", "1234567") != 0);        /* < 8 */
	CHECK(v(1, "example\nssid", "12345678") != 0);     /* control char in ssid */
	CHECK(v(1, "example-ssid", "pass\tword1") != 0);    /* control char in passphrase */
	CHECK(v(1, "example-ssid", "p\xc3\xa4sswort1") != 0); /* non-ASCII passphrase */

	/* test decisions from the supplicant's observations */
	{
		wifi_obs o;
		memset(&o, 0, sizeof(o));
		CHECK(wifi_test_decide(3000, &o) == WT_PENDING);
		CHECK(wifi_test_decide(17999, &o) == WT_PENDING);
		CHECK(wifi_test_decide(18000, &o) == WT_NOT_FOUND);       /* never associated */
		o.assoc_seen = 1;
		CHECK(wifi_test_decide(18000, &o) == WT_PENDING);         /* associated: wait */
		CHECK(wifi_test_decide(22000, &o) == WT_ERROR);           /* assoc, AP never sent msg-1 */
		o.msg1 = 1;
		CHECK(wifi_test_decide(21999, &o) == WT_PENDING);
		CHECK(wifi_test_decide(22000, &o) == WT_WRONG_PASS);      /* msg-2 sent, never a msg-3 */
		o.wrong = 1;
		CHECK(wifi_test_decide(5000, &o) == WT_WRONG_PASS);       /* AP refused our msg-2 */
		o.port_open = 1;
		CHECK(wifi_test_decide(5000, &o) == WT_OK);               /* port open wins */
		memset(&o, 0, sizeof(o));
		o.msg1 = 1;
		CHECK(wifi_test_decide(18000, &o) == WT_PENDING);         /* msg-1 seen: not "not found" */
		/* L3: at the limit, a MIC-verified msg-3 means the passphrase was right */
		memset(&o, 0, sizeof(o));
		o.assoc_seen = 1; o.msg1 = 2; o.msg3_ok = 1;
		CHECK(wifi_test_decide(22000, &o) == WT_ERROR);
		CHECK(strcmp(wifi_test_name(WT_NOT_FOUND), "not_found") == 0);
		CHECK(strcmp(wifi_test_name(WT_NO_DHCP), "no_dhcp") == 0);
		CHECK(strcmp(wifi_test_name(WT_WRONG_PASS), "wrong_passphrase") == 0);
		CHECK(strcmp(wifi_test_name(WT_BUSY), "busy") == 0);
	}
	/* uplink bring-up waits for the port before DHCP; a pending uplink change
	 * (cable back, wlan0 requested) must end that wait at once */
	polls = 0; abort_after = 2;
	CHECK(wifi_wait_port(15000, 1, abort_soon, never_open) == -1 && polls == 2);
	polls = 0;
	CHECK(wifi_wait_port(15000, 1, NULL, open_third) == 1 && polls == 3);
	polls = 0;
	CHECK(wifi_wait_port(10, 1, NULL, never_open) == 0);
	printf("wifi tests: %d checks, %d failures\n", checks, fails);
	return fails != 0;
}
