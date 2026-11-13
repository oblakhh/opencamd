/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * gpio — reset button and switch port link status via /dev/mem (see gpio.h).
 */

#include "gpio.h"

#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

/* GPIO_BASE 0xB8003500 (KSEG1) -> physical 0x18003500, within page 0x18003000. */
#define GPIO_PAGE  0x18003000u
#define GPIO_OFF   0x500u
#define R_CNR      0x00u    /* control: 0 = GPIO function */
#define R_DIR      0x08u    /* direction: 0 = input, 1 = output */
#define R_DAT      0x0Cu    /* data */
#define RESET_BIT  (1u << 3)   /* GPIOA3, active-low */

static volatile unsigned char *g_base;

static volatile uint32_t *reg(unsigned off)
{
	return (volatile uint32_t *)(g_base + GPIO_OFF + off);
}

int gpio_reset_open(void)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	void *m;
	if (fd < 0)
		return -1;
	m = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, GPIO_PAGE);
	close(fd);
	if (m == MAP_FAILED)
		return -1;
	g_base = m;
	*reg(R_CNR) &= ~RESET_BIT;   /* GPIO function */
	*reg(R_DIR) &= ~RESET_BIT;   /* input */
	return 0;
}

#define SW_PAGE    0x1B804000u   /* switch core 0xBB800000 + PCRAM 0x4100 */
#define PSRP0_OFF  0x128u
#define PS_LINKUP  (1u << 4)

static volatile unsigned char *g_sw;

static int sw_map(void)
{
	int fd;
	void *m;
	if (g_sw)
		return 0;
	fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0)
		return -1;
	m = mmap(0, 4096, PROT_READ, MAP_SHARED, fd, SW_PAGE);
	close(fd);
	if (m == MAP_FAILED)
		return -1;
	g_sw = m;
	return 0;
}

int eth_port_status(int port, unsigned *val)
{
	if (port < 0 || port > 4 || sw_map() != 0)
		return -1;
	*val = *(volatile uint32_t *)(g_sw + PSRP0_OFF + 4u * (unsigned)port);
	return 0;
}

int eth_link_up(void)
{
	unsigned v;
	int p;
	for (p = 0; p < 5; p++)
		if (eth_port_status(p, &v) == 0 && (v & PS_LINKUP))
			return 1;
	return 0;
}

int gpio_reset_pressed(void)
{
	if (!g_base)
		return 0;
	return (*reg(R_DAT) & RESET_BIT) == 0;   /* active-low */
}
