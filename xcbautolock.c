/*-
 * Copyright (c) 2015-2016 Baptiste Daroussin <bapt@FreeBSD.org>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/screensaver.h>

extern char **environ;

static int
parse_time(const char *time)
{
	intmax_t ret;
	char *end;

	ret = strtol(time, &end, 10);
	if (ret == 0 && end == time)
		errx(EXIT_FAILURE, "invalid time");

	if (end != NULL && *(end + 1) != '\0')
		errx(EXIT_FAILURE, "invalid time");

	switch (*end) {
	case 's':
		ret *= 1000;
		break;
	case 'm':
		ret *= 60 * 1000;
		break;
	case 'h':
		ret *= 60 * 60 * 1000;
		break;
	}

	if (ret < 0 || ret >= INT_MAX)
		errx(EXIT_FAILURE, "invalid time");

	return (ret);
}

static void
do_lock(int argc, char **argv)
{
	int error, i, pstat;
	pid_t pid;

	error = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
	if (error != 0) {
		fprintf(stderr, "Cannot run: '");
		for (i = 0; i < argc; i++) {
			fprintf(stderr, "%s%s", i == 0 ? "" : " ", argv[i]);
		}
		fprintf(stderr, "': %s\n", strerror(error));
		exit(EXIT_FAILURE);
	}

	while (waitpid(pid, &pstat, 0) == -1) {
		if (errno != EINTR) {
			err(EXIT_FAILURE, "Unexpected failure");
		}
	}

	return;
}

int
main(int argc, char **argv)
{
	int ch, time, timeout, screensaver_response_type = -1;
	pid_t pid;
	xcb_connection_t *conn;
	xcb_screen_t *screen;
	xcb_get_property_cookie_t pcookie;
	xcb_get_property_reply_t *preply;
	xcb_generic_event_t *event;
	const xcb_query_extension_reply_t *reply_screensaver;
	xcb_screensaver_query_info_cookie_t cookie;
	xcb_screensaver_query_info_reply_t *reply;
	xcb_screensaver_notify_event_t *ev;
	xcb_intern_atom_reply_t *atom;
	const xcb_setup_t * xcb_setup;
	int error = EXIT_SUCCESS;
	bool no_daemonize = false, forced = false, justunlocked = false;
	struct pollfd pfds[1];

	/* Default on 1min */
	time = 60000;

	while ((ch = getopt(argc, argv, "ft:")) != -1) {
		switch (ch) {
		case 't':
			time = parse_time(optarg);
			break;
		case 'f':
			no_daemonize = true;
			break;
		default:
			fprintf(stderr, "xcbautolock [-t duration] <command>\n");
			exit(1);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc == 0)
		errx(EXIT_FAILURE, "No locker specified");

	conn = xcb_connect(NULL, NULL);
	if (conn == NULL)
		errx(EXIT_FAILURE, "Not able to connect to the X session");
	xcb_setup = xcb_get_setup(conn);
	if (xcb_setup == NULL) {
		warnx("Not able to setup X connection");
		error = EXIT_FAILURE;
		goto disconnect;
	}
	screen = xcb_setup_roots_iterator(xcb_setup).data;
	atom = xcb_intern_atom_reply(conn,
	    xcb_intern_atom(conn, 0, sizeof("XLOCKER_PID"), "XLOCKER_PID"),
	    0);
	pcookie = xcb_get_property(conn, false, screen->root, atom->atom,
	    XCB_GET_PROPERTY_TYPE_ANY, 0, sizeof(pid));
	preply = xcb_get_property_reply(conn, pcookie, NULL);
	if (preply->type == XCB_ATOM_INTEGER) {
		memcpy(&pid, xcb_get_property_value(preply),
			xcb_get_property_value_length(preply));
		if (pid > 0 && !kill(pid, 0)) {
			free(preply);
			error = EXIT_FAILURE;
			warnx("Find running pid: %d\n", (int)pid);
			goto disconnect;
		}
	}
	free(preply);
	if (!no_daemonize)
		if (daemon(1, 0) == -1) {
			error = EXIT_FAILURE;
			warn("Fail to daemonize");
			goto disconnect;
		}

	pid = getpid();
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root,
	    atom->atom, XCB_ATOM_INTEGER, 32, 1, &pid);

	pfds[0].fd = xcb_get_file_descriptor(conn);
	pfds[0].events = POLLIN;

	/* Register for 'xset s' events */
	xcb_screensaver_select_input(conn, screen->root, XCB_SCREENSAVER_EVENT_NOTIFY_MASK);

	reply_screensaver = xcb_get_extension_data(conn, &xcb_screensaver_id);
	if (reply_screensaver)
		screensaver_response_type = reply_screensaver->first_event + XCB_SCREENSAVER_NOTIFY;

	for (;;) {
		cookie = xcb_screensaver_query_info(conn, screen->root);
		reply = xcb_screensaver_query_info_reply(conn, cookie, NULL);
		if (reply->state != XCB_SCREENSAVER_STATE_DISABLED || forced) {
			timeout = time - reply->ms_since_user_input;
			if ((reply->ms_since_user_input > time) || forced){
				do_lock(argc, argv);
				timeout = time;
				forced = false;
				justunlocked = true;
			}
		}
		free(reply);

		poll(pfds, 1, timeout);
		if (pfds[0].revents & POLLIN) {
			event = xcb_poll_for_event (conn);
			if (event) {
				if ((event->response_type & ~0x80) == screensaver_response_type) {
					ev = (xcb_screensaver_notify_event_t *)event;
					if (ev->state == XCB_SCREENSAVER_STATE_ON && !justunlocked) {
						forced = true;
					}
				}
			}
			justunlocked = false;
		}
	}

disconnect:
	xcb_disconnect(conn);

	return error;
}
