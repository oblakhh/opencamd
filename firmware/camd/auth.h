/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * auth — admin authentication for the control plane.
 * Two ways in (per the design): a session cookie for the UI (set by /api/login)
 * and HTTP Basic for integrations/scripts. Single admin account. Thread-safe;
 * civetweb serves requests on multiple worker threads.
 *
 * M3: password defaults to "admin" and lives only in memory. M8 adds a hashed,
 * config-stored password and a change endpoint — auth_set_password() is the seam.
 */
#ifndef CAMD_AUTH_H
#define CAMD_AUTH_H

#include <stddef.h>

void auth_init(void);
void auth_set_password(const char *pw);        /* hash+salt a new password */
void auth_set_password_hash(const char *stored); /* load stored "salt:hash" (boot) */
void auth_get_password_hash(char *out, size_t n); /* current "salt:hash" for persisting */
void auth_write_system_passwd(const char *path);  /* write busybox login passwd (telnet) */
int  auth_check_password(const char *pw);      /* 1 if matches */

/* Session cookies. Returns 1 and fills token (>= 33 bytes) on login. */
int  auth_login(char *token, size_t n);
void auth_logout(const char *token);
int  auth_valid_session(const char *token);    /* 1 if a live session */

/* Validate an "Authorization: Basic ..." header value. 1 if admin. */
int  auth_check_basic(const char *authz_header);

#endif
