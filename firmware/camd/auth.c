/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * auth — admin password hashing (crypt), session tokens and HTTP Basic
 * checking for the control plane (see auth.h).
 */

#include "auth.h"

#include <crypt.h>

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define USER      "admin"
#define MAX_SESS  8
#define TOKLEN    32   /* hex chars */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_pwhash[128];   /* crypt "$1$salt$hash"; empty = default "admin" */
static const char *DEFAULT_CRYPT = "$1$camdsalt$6XyBGDU.0z8wh/eJlcinz0"; /* crypt("admin") */
static char g_sess[MAX_SESS][TOKLEN + 1];
static int  g_next;

void auth_init(void)
{
	pthread_mutex_lock(&g_lock);
	memset(g_sess, 0, sizeof(g_sess));
	g_next = 0;
	pthread_mutex_unlock(&g_lock);
}

static void get_random(unsigned char *buf, int n)
{
	int fd = open("/dev/urandom", O_RDONLY), i;
	if (fd < 0 || read(fd, buf, n) != (ssize_t)n) {
		unsigned s = (unsigned)time(NULL) ^ (unsigned)getpid();
		for (i = 0; i < n; i++) { s = s * 1103515245u + 12345u; buf[i] = (unsigned char)(s >> 16); }
	}
	if (fd >= 0) close(fd);
}

static const char SALTCH[] =
    "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

/* build a "$1$XXXXXXXX$" md5crypt salt spec */
static void gen_salt(char *out, size_t n)
{
	unsigned char r[8];
	int i;
	if (n < 13) { out[0] = '\0'; return; }
	get_random(r, sizeof(r));
	out[0] = '$'; out[1] = '1'; out[2] = '$';
	for (i = 0; i < 8; i++) out[3 + i] = SALTCH[r[i] & 63];
	out[11] = '$'; out[12] = '\0';
}

void auth_set_password(const char *pw)
{
	char salt[16], *h;
	gen_salt(salt, sizeof(salt));
	pthread_mutex_lock(&g_lock);
	h = crypt(pw ? pw : "", salt);   /* not reentrant: under g_lock */
	snprintf(g_pwhash, sizeof(g_pwhash), "%s", h ? h : "");
	pthread_mutex_unlock(&g_lock);
}

void auth_set_password_hash(const char *stored)
{
	pthread_mutex_lock(&g_lock);
	if (stored && stored[0] == '$')          /* a crypt hash */
		snprintf(g_pwhash, sizeof(g_pwhash), "%s", stored);
	else
		g_pwhash[0] = '\0';                  /* empty/legacy -> default password */
	pthread_mutex_unlock(&g_lock);
}

void auth_get_password_hash(char *out, size_t n)
{
	pthread_mutex_lock(&g_lock);
	snprintf(out, n, "%s", g_pwhash);
	pthread_mutex_unlock(&g_lock);
}

/* Write a one-line passwd for busybox login/telnetd so the telnet shell shares
 * the admin password. /etc/passwd is a symlink to this (ramfs) file. */
void auth_write_system_passwd(const char *path)
{
	char hash[128];
	FILE *f;
	pthread_mutex_lock(&g_lock);
	snprintf(hash, sizeof(hash), "%s", g_pwhash[0] ? g_pwhash : DEFAULT_CRYPT);
	pthread_mutex_unlock(&g_lock);
	f = fopen(path, "w");
	if (!f) return;
	fprintf(f, "%s:%s:0:0:admin:/:/bin/sh\n", USER, hash);
	fclose(f);
}

/* Run a busybox tool without inheriting camd's descriptors (HTTP socket, video
 * device, flash): telnetd daemonizes and must not keep those open across a camd
 * restart. Waits for the direct child only. */
static void run_clean(const char *path, char *const argv[])
{
	pid_t p = fork();
	if (p == 0) {
		int fd;
		for (fd = 3; fd < 1024; fd++)
			close(fd);
		execv(path, argv);
		_exit(127);
	}
	if (p > 0)
		waitpid(p, NULL, 0);
}

/* Start or stop the telnet shell to match the setting (system.telnet). */
void auth_apply_telnet(int on)
{
	static int state = -1;   /* unknown at start: rcS no longer starts telnetd */
	char *start[] = { "telnetd", NULL };
	char *stop[] = { "killall", "telnetd", NULL };
	on = on ? 1 : 0;
	if (on == state)
		return;
	if (on)
		run_clean("/bin/telnetd", start);
	else
		run_clean("/bin/killall", stop);
	state = on;
}

int auth_check_password(const char *pw)
{
	char stored[128], *h;
	int ok;
	if (!pw)
		return 0;
	pthread_mutex_lock(&g_lock);
	snprintf(stored, sizeof(stored), "%s", g_pwhash);
	if (stored[0] == '\0') {
		ok = (strcmp(pw, "admin") == 0);          /* no password set -> default */
	} else {
		h = crypt(pw, stored);                    /* salt == the stored hash */
		ok = h && strcmp(h, stored) == 0;
	}
	pthread_mutex_unlock(&g_lock);
	return ok;
}

static void gen_token(char *tok)
{
	unsigned char raw[TOKLEN / 2];
	int i;
	get_random(raw, sizeof(raw));
	for (i = 0; i < (int)sizeof(raw); i++)
		sprintf(tok + i * 2, "%02x", raw[i]);
	tok[TOKLEN] = '\0';
}

int auth_login(char *token, size_t n)
{
	if (n < TOKLEN + 1)
		return 0;
	gen_token(token);
	pthread_mutex_lock(&g_lock);
	memcpy(g_sess[g_next], token, TOKLEN + 1); /* ring: oldest session evicted */
	g_next = (g_next + 1) % MAX_SESS;
	pthread_mutex_unlock(&g_lock);
	return 1;
}

void auth_logout(const char *token)
{
	int i;
	if (!token)
		return;
	pthread_mutex_lock(&g_lock);
	for (i = 0; i < MAX_SESS; i++)
		if (g_sess[i][0] && strcmp(g_sess[i], token) == 0)
			g_sess[i][0] = '\0';
	pthread_mutex_unlock(&g_lock);
}

int auth_valid_session(const char *token)
{
	int i, ok = 0;
	if (!token || !token[0])
		return 0;
	pthread_mutex_lock(&g_lock);
	for (i = 0; i < MAX_SESS; i++)
		if (g_sess[i][0] && strcmp(g_sess[i], token) == 0) { ok = 1; break; }
	pthread_mutex_unlock(&g_lock);
	return ok;
}

static int b64val(int c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

int auth_check_basic(const char *authz)
{
	char dec[160];
	int n = 0, val = 0, bits = 0;
	const char *p;
	char *colon;

	if (!authz || strncmp(authz, "Basic ", 6) != 0)
		return 0;
	for (p = authz + 6; *p && *p != '='; p++) {
		int v = b64val((unsigned char)*p);
		if (v < 0) continue;
		val = (val << 6) | v; bits += 6;
		if (bits >= 8) { bits -= 8; if (n < (int)sizeof(dec) - 1) dec[n++] = (char)((val >> bits) & 0xFF); }
	}
	dec[n] = '\0';
	colon = strchr(dec, ':');
	if (!colon)
		return 0;
	*colon = '\0';
	return strcmp(dec, USER) == 0 && auth_check_password(colon + 1);
}
