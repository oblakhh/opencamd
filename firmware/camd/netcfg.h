/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * netcfg — wired network config (DHCP / static on eth0) with safe apply.
 *
 * A change is applied live (shortly after the HTTP response is sent) but only
 * persisted when confirmed; without confirmation it reverts to the committed
 * config after a timeout. Nothing unconfirmed is ever saved, so a reboot also
 * reverts. The actual ifconfig/route/udhcpc work is done by /etc/netapply.sh,
 * executed via execv with validated arguments only.
 */
#ifndef CAMD_NETCFG_H
#define CAMD_NETCFG_H
#include "config.h"

int  netcfg_validate(const wired_cfg *w, const char **err); /* 0 = ok */
void netcfg_boot(const wired_cfg *committed, const char *hostname); /* apply static at start */
int  netcfg_try(const wired_cfg *w, const wired_cfg *committed, const char *hostname,
                int revert_s);                    /* schedule live apply + revert timer */
int  netcfg_confirm(wired_cfg *out);              /* 1 if a pending change was confirmed */
int  netcfg_pending_left(void);                   /* seconds until revert, 0 if none */
void netcfg_start(void);                          /* start the apply/revert thread */
int  netcfg_reapply(const wired_cfg *w, const char *hostname); /* apply now (uplink switch) */
int  netcfg_run(char *const argv[]);              /* execv /bin/sh argv, wait, 0 = ok */
#endif
