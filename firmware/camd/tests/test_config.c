/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * Host tests for the flash config store, using a file as fake flash.
 */
#include "config.h"
#include "config_store.h"
#include "flash.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CFG_BASE 0xC000u
#define CFG_SLOT 0x2000u
#define IMG_SIZE 0x10000u
#define IMG "test_flash.img"

static int tests, fails;
#define CHECK(c) do { tests++; if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void make_blank_flash(void)
{
	FILE *f = fopen(IMG, "wb");
	unsigned char *buf = malloc(IMG_SIZE);
	memset(buf, 0xFF, IMG_SIZE);
	fwrite(buf, 1, IMG_SIZE, f);
	fclose(f);
	free(buf);
}

static uint32_t read_u32(uint32_t abs_off)
{
	FILE *f = fopen(IMG, "rb");
	uint32_t v = 0;
	fseek(f, abs_off, SEEK_SET);
	if (fread(&v, 4, 1, f) != 1) v = 0;
	fclose(f);
	return v;
}
static void clobber(uint32_t abs_off)
{
	FILE *f = fopen(IMG, "r+b");
	fseek(f, abs_off, SEEK_SET);
	fputs("XXXX", f); /* trash the magic */
	fclose(f);
}

int main(void)
{
	config c;

	/* t1: blank flash -> defaults */
	make_blank_flash();
	CHECK(config_store_load(&c, IMG) == 1);
	CHECK(c.video.width == 1280 && strcmp(c.system.hostname, "ic3115w") == 0);

	/* t2: json round-trip */
	{
		config d;
		char *j;
		config_defaults(&c);
		strcpy(c.system.hostname, "printer");
		c.video.quality = 55;
		c.system.telnet = 0;
		j = config_to_json(&c, 1);
		CHECK(j != NULL);
		config_defaults(&d);
		CHECK(config_from_json(&d, j, 1) == 0);
		CHECK(strcmp(d.system.hostname, "printer") == 0 && d.video.quality == 55 && d.system.telnet == 0);
		free(j);
	}

	/* t3: save then load round-trips */
	config_defaults(&c);
	strcpy(c.system.hostname, "aaa");
	c.video.width = 640; c.video.height = 480;
	CHECK(config_store_save(&c, IMG) == 0);
	memset(&c, 0, sizeof(c));
	CHECK(config_store_load(&c, IMG) == 0);
	CHECK(strcmp(c.system.hostname, "aaa") == 0 && c.video.width == 640);

	/* t4: second save wins */
	config_defaults(&c);
	strcpy(c.system.hostname, "bbb");
	CHECK(config_store_save(&c, IMG) == 0);
	config_store_load(&c, IMG);
	CHECK(strcmp(c.system.hostname, "bbb") == 0);

	/* t5: corrupt the newest slot -> load falls back to the previous one (aaa) */
	{
		uint32_t seqA = read_u32(CFG_BASE + 8);
		uint32_t seqB = read_u32(CFG_BASE + CFG_SLOT + 8);
		clobber(seqA >= seqB ? CFG_BASE : CFG_BASE + CFG_SLOT);
		config_store_load(&c, IMG);
		CHECK(strcmp(c.system.hostname, "aaa") == 0); /* older slot survives */
	}

	/* t6: write guard — a region based below 0x8000 must refuse writes */
	{
		flash_region r;
		unsigned char buf[16] = {0};
		CHECK(flash_open(&r, IMG, 0x6000, 0x1000, 4096, 0x8000) == 0);
		CHECK(flash_erase(&r, 0, 4096) == -1);     /* abs 0x6000 < guard */
		CHECK(flash_write(&r, 0, buf, 16) == -1);
		flash_close(&r);
	}

	unlink(IMG);
	printf("config tests: %d checks, %d failures\n", tests, fails);
	return fails ? 1 : 0;
}
