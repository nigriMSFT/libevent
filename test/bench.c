/*
 * Copyright 2003-2007 Niels Provos <provos@citi.umich.edu>
 * Copyright 2007-2012 Niels Provos and Nick Mathewson
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *
 * Mon 03/10/2003 - Modified by Davide Libenzi <davidel@xmailserver.org>
 *
 *     Added chain event propagation to improve the sensitivity of
 *     the measure respect to the event loop efficency.
 *
 *
 */

#include "event2/event-config.h"
#include "../util-internal.h"

#include <sys/types.h>
#include <sys/stat.h>
#ifdef EVENT__HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/socket.h>
#include <signal.h>
#include <sys/resource.h>
#include <arpa/inet.h>
#endif
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef EVENT__HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>

#ifdef _WIN32
#include <getopt.h>
#endif

#include <event.h>
#include <evutil.h>
#include <event2/listener.h>

static ev_ssize_t count, fired;
static int writes, failures;
static evutil_socket_t *pipes;
static int num_pipes, num_active, num_writes;
static struct event *events;
static struct event_base *base;
static unsigned short server_port = 4444;


static void
read_cb(evutil_socket_t fd, short which, void *arg)
{
	ev_intptr_t idx = (ev_intptr_t) arg, widx = idx + 1;
	unsigned char ch;
	ev_ssize_t n;

	n = recv(fd, (char*)&ch, sizeof(ch), 0);
	if (n >= 0)
		count += n;
	else
		failures++;
	if (writes) {
		if (widx >= num_pipes)
			widx -= num_pipes;
		n = send(pipes[widx], "e", 1, 0);
		if (n != 1)
			failures++;
		writes--;
		fired++;
	}
}

static struct timeval *
run_once(void)
{
	long i, space;
	static struct timeval ts, te;

	for (i = 0; i < num_pipes; i++) {
		if (event_initialized(&events[i]))
			event_del(&events[i]);
		event_assign(&events[i], base, pipes[i], EV_READ | EV_PERSIST, read_cb, (void *)(ev_intptr_t) i);
		event_add(&events[i], NULL);
	}

	event_base_loop(base, EVLOOP_ONCE | EVLOOP_NONBLOCK);

	fired = 0;
	space = num_pipes / num_active;
	for (i = 0; i < num_active; i++, fired++)
		(void) send(pipes[i * space], "e", 1, 0);

	count = 0;
	writes = num_writes;
	{
		int xcount = 0;
		evutil_gettimeofday(&ts, NULL);
		do {
			event_base_loop(base, EVLOOP_ONCE | EVLOOP_NONBLOCK);
			xcount++;
		} while (count != fired);
		evutil_gettimeofday(&te, NULL);

		if (xcount != count)
			fprintf(stderr, "Xcount: %d, Rcount: " EV_SSIZE_FMT "\n",
				xcount, count);
	}

	evutil_timersub(&te, &ts, &te);

	return (&te);
}

static int
create_conn(evutil_socket_t *cp, struct sockaddr_in *sin)
{
	*cp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (connect(*cp, (struct sockaddr*)sin, sizeof(*sin))) {
		int err = EVUTIL_SOCKET_ERROR();
		fprintf(stderr, "connect: %d (%s)\n",
			err, evutil_socket_error_to_string(err));
		return -1;
	}
	return 0;
}

static void
client_run(struct sockaddr_in *sin)
{
	int i;
	struct timeval *tv;

	events = calloc(num_pipes, sizeof(struct event));
	pipes = calloc(num_pipes, sizeof(evutil_socket_t));
	if (events == NULL || pipes == NULL) {
		perror("calloc");
		exit(1);
	}

	for (i = 0; i < num_pipes; i++) {
		if (create_conn(&pipes[i], sin) == -1) {
			perror("pipe");
			exit(1);
		}
	}

	for (i = 0; i < 25; i++) {
		tv = run_once();
		fprintf(stdout, "%ld\n", tv->tv_sec * 1000000L + tv->tv_usec);
	}
}

static void
server_read_cb(evutil_socket_t fd, short which, void *arg)
{
	unsigned char ch;
	ev_ssize_t n;

	n = recv(fd, (char*)&ch, sizeof(ch), 0);
	if (n == 0) {
		event_del((struct event*)arg);
		evutil_closesocket(fd);
	}
	else if (n < 0) {
		perror("recv");
		event_del((struct event*)arg);
		evutil_closesocket(fd);
	}
	else
		n = send(fd, "e", 1, 0);
}

static void
accept_conn_cb(struct evconnlistener *listener, evutil_socket_t fd,
	struct sockaddr *address, int socklen, void *ctx)
{
	evutil_make_socket_nonblocking(fd);

	event_add(
		event_new(base, fd, EV_READ | EV_PERSIST, server_read_cb,
			event_self_cbarg()), NULL);
}

static void
accept_error_cb(struct evconnlistener *listener, void *arg)
{
	int err = EVUTIL_SOCKET_ERROR();
	fprintf(stderr, "accept_error_cb %d (%s) Shutting down.\n",
		err, evutil_socket_error_to_string(err));

	event_base_loopexit(base, NULL);
}

static void
server_run(void)
{
	struct evconnlistener *l;
	struct sockaddr_in sin;

	base = event_base_new();

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(0);
	sin.sin_port = htons(server_port);

	l = evconnlistener_new_bind(
		base, accept_conn_cb, NULL, LEV_OPT_CLOSE_ON_FREE|LEV_OPT_REUSEABLE, -1,
		(struct sockaddr*)&sin, sizeof(sin));
	if (!l) {
		perror("evconnlistener_new_bind");
		return;
	}
	evconnlistener_set_error_cb(l, accept_error_cb);

	event_base_dispatch(base);
}

int
main(int argc, char **argv)
{
#ifdef EVENT__HAVE_SETRLIMIT
	struct rlimit rl;
#endif
	int i, c;
	const char **methods;
	const char *method = NULL;
	struct event_config *cfg = NULL;
	int server = 0;
	struct sockaddr_in sin;

#ifdef _WIN32
	WSADATA WSAData;
	WSAStartup(0x101, &WSAData);
#endif
	num_pipes = 100;
	num_active = 1;
	num_writes = num_pipes;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(server_port);
	while ((c = getopt(argc, argv, "n:a:w:m:c:ls")) != -1) {
		switch (c) {
		case 'n':
			num_pipes = atoi(optarg);
			break;
		case 'a':
			num_active = atoi(optarg);
			break;
		case 'w':
			num_writes = atoi(optarg);
			break;
		case 'm':
			method = optarg;
			break;
		case 's':
			server = 1;
			break;
		case 'c':
			inet_pton(AF_INET, optarg, &sin.sin_addr);
			break;
		case 'l':
			methods = event_get_supported_methods();
			fprintf(stdout, "Using Libevent %s. Available methods are:\n",
				event_get_version());
			for (i = 0; methods[i] != NULL; ++i)
				printf("    %s\n", methods[i]);
			exit(0);
		default:
			fprintf(stderr, "Illegal argument \"%c\"\n", c);
			exit(1);
		}
	}

#ifdef EVENT__HAVE_SETRLIMIT
	rl.rlim_cur = rl.rlim_max = 65536;
	if (setrlimit(RLIMIT_NOFILE, &rl) == -1) {
		perror("setrlimit");
		exit(1);
	}
#endif

	if (method != NULL) {
		cfg = event_config_new();
		methods = event_get_supported_methods();
		for (i = 0; methods[i] != NULL; ++i)
			if (strcmp(methods[i], method))
				event_config_avoid_method(cfg, methods[i]);
		base = event_base_new_with_config(cfg);
		event_config_free(cfg);
	} else
		base = event_base_new();

	if (server)
		server_run();
	else
		client_run(&sin);

	exit(0);
}
