/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * uploader — periodic HTTP(S) push of the current JPEG to a configured
 * endpoint (see uploader.h).
 */

#include "uploader.h"
#include "log.h"
#include "version.h"
#include "ota.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>

#ifdef HAVE_TLS
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#endif

/* Split "http://host[:port]/path" into host/port/path. Returns 0 on success;
 * rejects https:// (no TLS). */
static int parse_url(const char *url, char *host, size_t hsz, int *port,
                     char *path, size_t psz)
{
	const char *p = url, *slash, *colon;
	size_t hostlen;

	if (strncmp(p, "http://", 7) == 0)
		p += 7;
	else if (strncmp(p, "https://", 8) == 0)
		return -1;                 /* no TLS support */
	slash = strchr(p, '/');
	colon = strchr(p, ':');
	*port = 80;
	if (colon && (!slash || colon < slash)) {
		hostlen = (size_t)(colon - p);
		*port = atoi(colon + 1);
	} else {
		hostlen = slash ? (size_t)(slash - p) : strlen(p);
	}
	if (hostlen == 0 || hostlen >= hsz || *port <= 0 || *port > 65535)
		return -1;
	memcpy(host, p, hostlen);
	host[hostlen] = '\0';
	snprintf(path, psz, "%s", slash ? slash : "/");
	return 0;
}

static int tcp_connect(const char *host, int port, int timeout_s)
{
	struct addrinfo hints, *res = NULL, *ai;
	char ports[8];
	int fd = -1;

	snprintf(ports, sizeof(ports), "%d", port);
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, ports, &hints, &res) != 0 || !res)
		return -1;
	for (ai = res; ai; ai = ai->ai_next) {
		int fl, rc;
		fd = socket(ai->ai_family, SOCK_STREAM, 0);
		if (fd < 0)
			continue;
		fl = fcntl(fd, F_GETFL, 0);
		fcntl(fd, F_SETFL, fl | O_NONBLOCK);
		rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
		if (rc == 0) { fcntl(fd, F_SETFL, fl); break; }
		if (rc < 0 && errno == EINPROGRESS) {
			fd_set wf;
			struct timeval tv;
			FD_ZERO(&wf); FD_SET(fd, &wf);
			tv.tv_sec = timeout_s; tv.tv_usec = 0;
			if (select(fd + 1, NULL, &wf, NULL, &tv) > 0) {
				int err = 0; socklen_t el = sizeof(err);
				getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
				if (err == 0) { fcntl(fd, F_SETFL, fl); break; }
			}
		}
		close(fd); fd = -1;
	}
	freeaddrinfo(res);
	if (fd >= 0) {
		struct timeval tv;
		tv.tv_sec = timeout_s; tv.tv_usec = 0;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	}
	return fd;
}

static int write_all(int fd, const void *buf, size_t n)
{
	const char *p = buf;
	while (n) {
		ssize_t w = write(fd, p, n);
		if (w <= 0) return -1;
		p += w; n -= (size_t)w;
	}
	return 0;
}

/* Case-insensitive substring search (for "user-agent:" detection). */
static const char *ci_find(const char *hay, const char *needle)
{
	size_t nl = strlen(needle);
	for (; *hay; hay++)
		if (strncasecmp(hay, needle, nl) == 0)
			return hay;
	return NULL;
}

/* Turn the newline-separated `headers` config into CRLF-terminated request
 * header lines (blank lines skipped). Output is "" or "Key: V\r\nKey2: V\r\n".
 * A default User-Agent (CAMD_UA) is appended unless the operator set one —
 * Prusa's Camera Service rejects requests without a User-Agent. */
static void build_headers(const char *in, char *out, size_t osz)
{
	size_t o = 0;
	const char *p = in;
	while (*p && o + 2 < osz) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		while (len && p[len - 1] == '\r') len--;      /* trim CR */
		if (len && o + len + 2 < osz) {
			memcpy(out + o, p, len); o += len;
			out[o++] = '\r'; out[o++] = '\n';
		}
		if (!nl) break;
		p = nl + 1;
	}
	out[o] = '\0';
	if (!ci_find(out, "user-agent:")) {
		int k = snprintf(out + o, osz - o, "User-Agent: %s\r\n", CAMD_UA);
		if (k > 0 && o + (size_t)k < osz)
			o += (size_t)k;
		else
			out[o] = '\0';   /* didn't fit: leave headers unchanged */
	}
}

/* Build the HTTP request header (minus body) for host/path. Returns length. */
static int build_request(char *hdr, size_t hsz, const char *method,
    const char *path, const char *host, const char *hblock, int multipart,
    const char *boundary, unsigned long body_len)
{
	if (multipart)
		return snprintf(hdr, hsz,
		    "%s %s HTTP/1.1\r\nHost: %s\r\n%sContent-Type: "
		    "multipart/form-data; boundary=%s\r\nContent-Length: %lu\r\n"
		    "Connection: close\r\n\r\n", method, path, host, hblock,
		    boundary, body_len);
	return snprintf(hdr, hsz,
	    "%s %s HTTP/1.1\r\nHost: %s\r\n%sContent-Type: image/jpeg\r\n"
	    "Content-Length: %lu\r\nConnection: close\r\n\r\n",
	    method, path, host, hblock, body_len);
}

#ifdef HAVE_TLS
static int ssl_send(mbedtls_ssl_context *ssl, const unsigned char *buf, size_t len)
{
	size_t off = 0;
	while (off < len) {
		int r = mbedtls_ssl_write(ssl, buf + off, len - off);
		if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (r <= 0)
			return -1;
		off += (size_t)r;
	}
	return 0;
}

/* HTTPS upload via mbedTLS (TLS 1.2). Certificate verification is disabled
 * (PrusaLink/Connect ship self-signed or redirect to storage) — same posture
 * as `curl -k`. SNI is set. Follows 3xx redirects (preserving method + body,
 * like `curl -L`) because Prusa Connect redirects an authenticated upload to
 * its storage backend. Returns the final HTTP status, or -1 on failure. */
static int ssl_read_headers(mbedtls_ssl_context *ssl, char *buf, size_t cap)
{
	size_t off = 0;
	while (off + 1 < cap) {
		int r = mbedtls_ssl_read(ssl, (unsigned char *)buf + off, cap - 1 - off);
		if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (r <= 0)
			break;
		off += (size_t)r;
		buf[off] = '\0';
		if (strstr(buf, "\r\n\r\n"))
			break;
	}
	buf[off] = '\0';
	return (int)off;
}

/* Case-insensitive extraction of a response header value. Returns 0 on hit. */
static int hdr_value(const char *resp, const char *name, char *out, size_t osz)
{
	const char *p = resp;
	size_t nlen = strlen(name);
	while (*p) {
		const char *eol = strstr(p, "\r\n");
		size_t llen = eol ? (size_t)(eol - p) : strlen(p);
		if (llen > nlen && strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
			const char *v = p + nlen + 1;
			size_t vlen;
			while (*v == ' ') v++;
			vlen = (size_t)((eol ? eol : p + llen) - v);
			if (vlen >= osz) vlen = osz - 1;
			memcpy(out, v, vlen); out[vlen] = '\0';
			return 0;
		}
		if (!eol) break;
		p = eol + 2;
	}
	return -1;
}

/* One TLS request. Fills location[] from a 3xx response. Returns status or -1. */
static int tls_request(const char *host, int port, const char *path,
    const char *method, const char *hblock, int multipart, const char *field,
    const unsigned char *jpg, size_t jlen, char *location, size_t locsz)
{
	char ports[8], hdr[1024], resp[1536];
	const char *pers = "camd-uploader";
	int code = -1, ret, n;
	mbedtls_net_context net;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;

	snprintf(ports, sizeof(ports), "%d", port);
	mbedtls_net_init(&net);
	mbedtls_ssl_init(&ssl);
	mbedtls_ssl_config_init(&conf);
	mbedtls_entropy_init(&entropy);
	mbedtls_ctr_drbg_init(&drbg);

	if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
	        (const unsigned char *)pers, strlen(pers)) != 0)
		goto done;
	if (mbedtls_net_connect(&net, host, ports, MBEDTLS_NET_PROTO_TCP) != 0)
		goto done;
	{
		struct timeval tv;
		tv.tv_sec = 20; tv.tv_usec = 0;
		setsockopt(net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(net.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	}
	if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
	        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0)
		goto done;
	mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);  /* -k */
	mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
	if (mbedtls_ssl_setup(&ssl, &conf) != 0)
		goto done;
	mbedtls_ssl_set_hostname(&ssl, host);   /* SNI */
	mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv, NULL);

	while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
		if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
		    ret != MBEDTLS_ERR_SSL_WANT_WRITE)
			goto done;
	}

	if (multipart) {
		const char *B = "camduploadboundary";
		char pre[512], post[64];
		int pn = snprintf(pre, sizeof(pre),
		    "--%s\r\nContent-Disposition: form-data; name=\"%s\"; "
		    "filename=\"snapshot.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n",
		    B, field);
		int on = snprintf(post, sizeof(post), "\r\n--%s--\r\n", B);
		unsigned long clen = (unsigned long)pn + jlen + (unsigned long)on;
		n = build_request(hdr, sizeof(hdr), method, path, host, hblock, 1, B, clen);
		if (ssl_send(&ssl, (unsigned char *)hdr, n) ||
		    ssl_send(&ssl, (unsigned char *)pre, pn) ||
		    ssl_send(&ssl, jpg, jlen) ||
		    ssl_send(&ssl, (unsigned char *)post, on))
			goto done;
	} else {
		n = build_request(hdr, sizeof(hdr), method, path, host, hblock, 0, NULL,
		    (unsigned long)jlen);
		if (ssl_send(&ssl, (unsigned char *)hdr, n) ||
		    ssl_send(&ssl, jpg, jlen))
			goto done;
	}

	ret = ssl_read_headers(&ssl, resp, sizeof(resp));
	if (ret > 0) {
		if (sscanf(resp, "HTTP/%*d.%*d %d", &code) != 1)
			code = -1;
		if (location && locsz)
			location[0] = '\0';
		if (code >= 300 && code < 400 && location)
			hdr_value(resp, "Location", location, locsz);
	}
	mbedtls_ssl_close_notify(&ssl);

done:
	mbedtls_ssl_free(&ssl);
	mbedtls_ssl_config_free(&conf);
	mbedtls_ctr_drbg_free(&drbg);
	mbedtls_entropy_free(&entropy);
	mbedtls_net_free(&net);
	return code;
}

static int send_via_tls(const upload_cfg *c, const unsigned char *jpg, size_t jlen)
{
	char url[512], host[160], path[256], hblock[512];
	const char *method = c->method[0] ? c->method : "POST";
	int multipart = (strcasecmp(method, "PUT") != 0) && c->field[0];
	int hops, code = -1;

	build_headers(c->headers, hblock, sizeof(hblock));
	snprintf(url, sizeof(url), "%s", c->url);

	for (hops = 0; hops < 5; hops++) {
		char location[512];
		const char *p, *slash, *colon;
		size_t hostlen;
		int port = 443;

		if (strncmp(url, "https://", 8) != 0) {
			LOGW("upload: redirect to non-https not followed (%.64s)", url);
			break;
		}
		p = url + 8;
		slash = strchr(p, '/');
		colon = strchr(p, ':');
		if (colon && (!slash || colon < slash)) {
			hostlen = (size_t)(colon - p);
			port = atoi(colon + 1);
		} else {
			hostlen = slash ? (size_t)(slash - p) : strlen(p);
		}
		if (hostlen == 0 || hostlen >= sizeof(host) || port <= 0 || port > 65535)
			break;
		memcpy(host, p, hostlen);
		host[hostlen] = '\0';
		snprintf(path, sizeof(path), "%s", slash ? slash : "/");

		location[0] = '\0';
		code = tls_request(host, port, path, method, hblock, multipart,
		    c->field, jpg, jlen, location, sizeof(location));

		if (code >= 300 && code < 400 && location[0]) {
			LOGI("upload: HTTP %d redirect -> %.96s", code, location);
			if (strncmp(location, "http", 4) == 0) {
				snprintf(url, sizeof(url), "%s", location);
			} else if (location[0] == '/') {
				char nu[512];
				if (port == 443)
					snprintf(nu, sizeof(nu), "https://%s%s", host, location);
				else
					snprintf(nu, sizeof(nu), "https://%s:%d%s", host, port, location);
				snprintf(url, sizeof(url), "%s", nu);
			} else {
				char nu[512];
				snprintf(nu, sizeof(nu), "https://%s/%s", host, location);
				snprintf(url, sizeof(url), "%s", nu);
			}
			continue;
		}
		break;
	}
	return code;
}

#endif /* HAVE_TLS */

/* Send one snapshot. Returns the HTTP status code, or -1 on transport error. */
static int send_once(const upload_cfg *c, const unsigned char *jpg, size_t jlen)
{
	char host[160], path[256], hdr[1024], hblock[512];
	const char *method = c->method[0] ? c->method : "POST";
	int port, fd, code = -1, n;
	int multipart = (strcasecmp(method, "PUT") != 0) && c->field[0];

	if (strncmp(c->url, "https://", 8) == 0) {
#ifdef HAVE_TLS
		return send_via_tls(c, jpg, jlen);
#else
		LOGW("upload: https requires a TLS build");
		return -1;
#endif
	}

	build_headers(c->headers, hblock, sizeof(hblock));

	if (parse_url(c->url, host, sizeof(host), &port, path, sizeof(path)) != 0) {
		LOGW("upload: bad url %s", c->url);
		return -1;
	}
	fd = tcp_connect(host, port, 8);
	if (fd < 0)
		return -1;

	if (multipart) {
		const char *B = "camduploadboundary";
		char pre[512], post[64];
		int pn = snprintf(pre, sizeof(pre),
		    "--%s\r\nContent-Disposition: form-data; name=\"%s\"; "
		    "filename=\"snapshot.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n",
		    B, c->field);
		int on = snprintf(post, sizeof(post), "\r\n--%s--\r\n", B);
		unsigned long clen = (unsigned long)pn + jlen + (unsigned long)on;
		n = snprintf(hdr, sizeof(hdr),
		    "%s %s HTTP/1.1\r\nHost: %s\r\n%sContent-Type: "
		    "multipart/form-data; boundary=%s\r\nContent-Length: %lu\r\n"
		    "Connection: close\r\n\r\n",
		    method, path, host, hblock, B, clen);
		if (write_all(fd, hdr, n) || write_all(fd, pre, pn) ||
		    write_all(fd, jpg, jlen) || write_all(fd, post, on)) {
			close(fd); return -1;
		}
	} else {
		n = snprintf(hdr, sizeof(hdr),
		    "%s %s HTTP/1.1\r\nHost: %s\r\n%sContent-Type: image/jpeg\r\n"
		    "Content-Length: %lu\r\nConnection: close\r\n\r\n",
		    method, path, host, hblock, (unsigned long)jlen);
		if (write_all(fd, hdr, n) || write_all(fd, jpg, jlen)) {
			close(fd); return -1;
		}
	}

	{
		char resp[128];
		ssize_t r = read(fd, resp, sizeof(resp) - 1);
		if (r > 0) {
			resp[r] = '\0';
			if (sscanf(resp, "HTTP/%*d.%*d %d", &code) != 1)
				code = -1;
		}
	}
	close(fd);
	return code;
}

static void *uploader_loop(void *arg)
{
	uploader *u = arg;
	while (u->running) {
		int interval = u->cfg->interval > 0 ? u->cfg->interval : 10;
		if (u->cfg->enabled && u->cfg->url[0] && !ota_in_progress) {
			frame_ref ref;
			if (framepool_acquire(u->pool, 0, 2000, &ref) == 0) {
				int code = send_once(u->cfg, ref.data, ref.len);
				framepool_release(u->pool, &ref);
				u->last_code = code;
				u->ok = (code >= 200 && code < 300);
				if (u->ok) u->count++;
				else LOGW("upload: %s -> %d", u->cfg->url, code);
			} else {
				u->last_code = 0;
				u->ok = 0;
			}
		}
		{ int i; for (i = 0; i < interval && u->running; i++) sleep(1); }
	}
	return NULL;
}

int uploader_start(uploader *u, const upload_cfg *cfg, framepool *pool)
{
	memset(u, 0, sizeof(*u));
	u->cfg = cfg;
	u->pool = pool;
	u->running = 1;
	if (pthread_create(&u->thread, NULL, uploader_loop, u) != 0) {
		LOGE("uploader: pthread_create failed");
		u->running = 0;
		return -1;
	}
	return 0;
}

void uploader_stop(uploader *u)
{
	if (!u->running)
		return;
	u->running = 0;
	pthread_join(u->thread, NULL);
}
