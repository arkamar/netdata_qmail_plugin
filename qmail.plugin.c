/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "callbacks.h"
#include "err.h"
#include "flush.h"
#include "signal.h"
#include "timer.h"
#include "vector.h"

#include "fs.h"
#include "send.h"
#include "smtp.h"

#define DEFAULT_PATH "/var/log/qmail"

enum poll {
	POLL_SIGNAL = 0,
	POLL_TIMER,
	POLL_FS_EVENT,
	POLL_LENGTH
};

#define LEN(x) ( sizeof x / sizeof * x )

static
void
usage(const char * name) {
	fprintf(stderr, "usage: %s <timout> [path]\n", name);
}

static
void
prepare_watcher(struct fs_event * watch, const int fd, const struct stat_func * func) {
	char file_name[PATH_MAX];
	sprintf(file_name, "%s/current", watch->dir_name);
	watch->watch_dir = inotify_add_watch(fd, watch->dir_name, IN_CREATE);
	if (watch->watch_dir == -1) {
		perror("inotify_add_watch");
		exit(1);
	}
	watch->fd = open(file_name, O_RDONLY);
	if (watch->fd == -1) {
		perror("open");
		exit(1);
	}
	lseek(watch->fd, 0, SEEK_END);
	watch->func = func;
	watch->data = func->init();
}

static
void
detect_log_dirs(const int fd, struct vector * v) {
	struct dirent * dir_entry;
	const char * dir_name;
	struct fs_event watch;
	DIR * dir;

	dir = opendir(".");
	if (dir == NULL) {
		perror("opendir");
		exit(1);
	}

	while ((dir_entry = readdir(dir))) {
		dir_name = dir_entry->d_name;

		if (dir_name[0] == '.')
			continue;

		if (is_directory(dir_name) == 1) {
			memset(&watch, 0, sizeof watch);
			if (strstr(dir_name, "send")) {
				fprintf(stderr, "send log directory detected: %s\n", dir_name);
				watch.dir_name = strdup(dir_name);
				prepare_watcher(&watch, fd, send_func);
				vector_add(v, &watch);
			} else if (strstr(dir_name, "smtp")) {
				fprintf(stderr, "smtp log directory detected: %s\n", dir_name);
				watch.dir_name = strdup(dir_name);
				prepare_watcher(&watch, fd, smtp_func);
				vector_add(v, &watch);
			} else
				fputs("I don't know\n", stderr);
		}
	}

	closedir(dir);
}

int
main(int argc, const char * argv[]) {
	struct pollfd pfd[POLL_LENGTH];
	struct vector vector = VECTOR_EMPTY;
	unsigned long last_update;
	const char * argv0;
	const char * path;
	int timeout = 1;
	int fs_event_fd;
	int signal_fd;
	int timer_fd;
	int run;
	int i;

	path = DEFAULT_PATH;
	argv0 = *argv; argv++; argc--;

	if (argc > 0) {
		timeout = atoi(*argv);
		argv++; argc--;
	} else
		usage(argv0);

	if (argc > 0) {
		path = *argv;
		argv++; argc--;
	}

	if (is_directory(path) < 1) {
		fprintf(stderr, "Cannot change dir to %s, it is not a directory\n", path);
		/* TODO: Maybe we can distinguish all return values
		 *  1 - it is a directory
		 *  0 - it is not a directory
		 * -1 - an error; file does not exit, etc.
		 */
		exit(1);
	}
	if (chdir(path) == -1) {
		fprintf(stderr, "Cannot change directory to '%s': %s\n", path, strerror(errno));
		exit(1);
	}

	vector_init(&vector, sizeof(struct fs_event));

	timer_fd = prepare_timer_fd(timeout);
	pfd[POLL_TIMER].fd = timer_fd;
	pfd[POLL_TIMER].events = POLLIN;

	signal_fd = prepare_signal_fd();
	pfd[POLL_SIGNAL].fd = signal_fd;
	pfd[POLL_SIGNAL].events = POLLIN;

	fs_event_fd = prepare_fs_event_fd();
	pfd[POLL_FS_EVENT].fd = fs_event_fd;
	pfd[POLL_FS_EVENT].events = POLLIN;

	detect_log_dirs(fs_event_fd, &vector);

	for (i = 0; i < vector.len; i++) {
		struct fs_event * watch = vector_item(&vector, i);
		watch->func->print_hdr(watch->dir_name);
		clock_gettime(CLOCK_REALTIME, &watch->time);
	}

	for (run = 1; run;) {
		switch (poll(pfd, LEN(pfd), -1)) {
		case -1:
			perror("poll");
			break;
		case 0:
			fputs("timeout\n", stderr);
			continue;
		default:
			if (pfd[POLL_SIGNAL].revents & POLLIN) {
				flush_read_fd(signal_fd);
				run = 0;
				continue;
			}
			if (pfd[POLL_FS_EVENT].revents & POLLIN) {
			/* TODO: dir event notifier:
			 *
			 * - it detects new directories in DEFAULT_PATH
			 * - it detects 'current' log rotations and reopes and reregister it again
			 * - it detects change in every registerd 'current' log and process appended lines
			 */
				fputs("fs event\n", stderr);
				process_fs_event_queue(fs_event_fd, vector.data, vector.len);
			}
			if (pfd[POLL_TIMER].revents & POLLIN) {
				fputs("D: time to print\n", stderr);
				flush_read_fd(timer_fd);
				for (i = 0; i < vector.len; i++) {
					struct fs_event * statistics = vector_item(&vector, i);

					read_log_file(statistics);

					if (statistics->func->postprocess)
						statistics->func->postprocess(statistics->data);

					last_update = update_timestamp(&statistics->time);
					statistics->func->print(statistics->dir_name, statistics->data, last_update);
					statistics->func->clear(statistics->data);
				}
			}
		}
	}

	for (i = 0; i < vector.len; i++) {
		struct fs_event * watch = vector_item(&vector, i);
		free((void *)watch->dir_name);
		watch->func->fini(watch->data);
		close(watch->fd);
	}
	close(fs_event_fd);
	close(timer_fd);
	close(signal_fd);

	return 0;
}
