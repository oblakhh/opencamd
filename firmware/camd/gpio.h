/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * gpio — reset button (GPIOA3) read via /dev/mem. Register polarity per the
 * RTL8196C bootcode: PABCDCNR bit 0 = GPIO function, PABCDDIR bit 0 = input,
 * button is active-low on PABCDDAT bit 3.
 */
#ifndef CAMD_GPIO_H
#define CAMD_GPIO_H
int gpio_reset_open(void);      /* map + configure GPIOA3 as input; 0 on success */
int gpio_reset_pressed(void);   /* 1 while the reset button is held */

/* Switch port status (read-only): PSRPn at phys 0x1B804128 + 4n, bit 4 = link.
 * The eth0 netdev's carrier never changes on this driver, so this is the only
 * reliable wired-link signal. */
int eth_port_status(int port, unsigned *val);   /* 0 ok; port 0..4 */
int eth_link_up(void);                          /* 1 if any port has link */
#endif
