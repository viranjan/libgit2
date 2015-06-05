/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#ifdef GIT_CURL

#include <curl/curl.h>

#include "stream.h"
#include "git2/transport.h"
#include "buffer.h"

typedef struct {
	git_stream parent;
	CURL *handle;
	curl_socket_t socket;
	char curl_error[CURL_ERROR_SIZE + 1];
	git_cert_x509 cert_info;
} curl_stream;

static int seterr_curl(curl_stream *s)
{
	giterr_set(GITERR_NET, "curl error: %s\n", s->curl_error);
	return -1;
}

static int curls_connect(git_stream *stream)
{
	curl_stream *s = (curl_stream *) stream;
	long sockextr;
	int failed_cert = 0;
	CURLcode res;
	res = curl_easy_perform(s->handle);

	if (res != CURLE_OK && res != CURLE_PEER_FAILED_VERIFICATION)
		return seterr_curl(s);
	if (res == CURLE_PEER_FAILED_VERIFICATION)
		failed_cert = 1;

	if ((res = curl_easy_getinfo(s->handle, CURLINFO_LASTSOCKET, &sockextr)) != CURLE_OK)
		return seterr_curl(s);

	s->socket = sockextr;

	if (s->parent.encrypted && failed_cert)
		return GIT_ECERTIFICATE;

	return 0;
}

static int curls_certificate(git_cert **out, git_stream *stream)
{
	curl_stream *s = (curl_stream *) stream;

	s->cert_info.cert_type = GIT_CERT_X509;
	s->cert_info.data      = NULL;
	s->cert_info.len       = 0;

	*out = (git_cert *) &s->cert_info;

	return 0;
}

static int wait_for(curl_socket_t fd, bool reading)
{
	int ret;
	fd_set infd, outfd, errfd;

	FD_ZERO(&infd);
	FD_ZERO(&outfd);
	FD_ZERO(&errfd);

	FD_SET(fd, &errfd);
	if (reading)
		FD_SET(fd, &infd);
	else
		FD_SET(fd, &outfd);

	if ((ret = select(fd + 1, &infd, &outfd, &errfd, NULL)) < 0) {
		giterr_set(GITERR_OS, "error in select");
		return -1;
	}

	return 0;
}

static ssize_t curls_write(git_stream *stream, const char *data, size_t len, int flags)
{
	int error;
	size_t off = 0, sent;
	CURLcode res;
	curl_stream *s = (curl_stream *) stream;

	GIT_UNUSED(flags);

	do {
		if ((error = wait_for(s->socket, false)) < 0)
			return error;

		res = curl_easy_send(s->handle, data + off, len - off, &sent);
		if (res == CURLE_OK)
			off += sent;
	} while ((res == CURLE_OK || res == CURLE_AGAIN) && off < len);

	if (res != CURLE_OK)
		return seterr_curl(s);

	return len;
}

static ssize_t curls_read(git_stream *stream, void *data, size_t len)
{
	int error;
	size_t read;
	CURLcode res;
	curl_stream *s = (curl_stream *) stream;

	do {
		if ((error = wait_for(s->socket, true)) < 0)
			return error;

		res = curl_easy_recv(s->handle, data, len, &read);
	} while (res == CURLE_AGAIN);

	if (res != CURLE_OK)
		return seterr_curl(s);

	return read;
}

static int curls_close(git_stream *stream)
{
	curl_stream *s = (curl_stream *) stream;

	if (!s->handle)
		return 0;

	curl_easy_cleanup(s->handle);
	s->handle = NULL;
	s->socket = 0;

	return 0;
}

static void curls_free(git_stream *stream)
{
	curl_stream *s = (curl_stream *) stream;

	curls_close(stream);
	git__free(s);
}

int git_curl_stream_new(git_stream **out, const char *host, const char *port, int encrypted)
{
	curl_stream *st;
	CURL *handle;
	int iport = 0, error;

	st = git__calloc(1, sizeof(curl_stream));
	GITERR_CHECK_ALLOC(st);

	handle = curl_easy_init();
	if (handle == NULL) {
		giterr_set(GITERR_NET, "failed to create curl handle");
		return -1;
	}

	if ((error = git__strtol32(&iport, port, NULL, 10)) < 0)
		return error;

	if (encrypted) {
		git_buf buf = GIT_BUF_INIT;
		git_buf_printf(&buf, "https://%s", host);
		curl_easy_setopt(handle, CURLOPT_URL, buf.ptr);
		git_buf_free(&buf);
	} else {
		curl_easy_setopt(handle, CURLOPT_URL, host);
	}

	curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, st->curl_error);
	curl_easy_setopt(handle, CURLOPT_PORT, iport);
	curl_easy_setopt(handle, CURLOPT_CONNECT_ONLY, 1);
	curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1);
	curl_easy_setopt(handle, CURLOPT_CERTINFO, 1);
	/* curl_easy_setopt(handle, CURLOPT_VERBOSE, 1); */

	st->parent.version = GIT_STREAM_VERSION;
	st->parent.encrypted = encrypted;
	st->parent.connect = curls_connect;
	st->parent.certificate = curls_certificate;
	st->parent.read = curls_read;
	st->parent.write = curls_write;
	st->parent.close = curls_close;
	st->parent.free = curls_free;
	st->handle = handle;

	*out = (git_stream *) st;
	return 0;
}

#else

#include "stream.h"

int git_curl_stream_new(git_stream **out, const char *host, const char *port)
{
	GIT_UNUSED(out);
	GIT_UNUSED(host);
	GIT_UNUSED(port);

	giterr_set(GITERR_NET, "curl is not supported in this version");
	return -1;
}


#endif
