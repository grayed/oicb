/*
 * Copyright (c) 2014-2020 Vadim Zhukov <zhuk@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "oicb.h"
#include "history.h"


struct history_files_list history_files;

static struct history_file	*get_history_file(char *path);
static char			*get_save_path_for(char type, const char *peer,
                                                   const char *msg);

int		 enable_history = 1;
char		 history_path[PATH_MAX];


static char*
get_save_path_for(char type, const char *peer, const char *msg) {
	int		 rv;
	const char	*prefix;
	char		*path;

#define NO_SUCH_USER	"No such user "
	if (type == 'e' &&
	    strncmp(msg, NO_SUCH_USER, strlen(NO_SUCH_USER)) == 0) {
		// Those errors occur happen in private chats,
		// so it's logical to save them there.
		peer = msg + strlen(NO_SUCH_USER);
		prefix = "private-";
	} else if (type != 'c') {
		peer = room;
		prefix = "room-";
	} else {
		prefix = "private-";
	}

	rv = asprintf(&path, "%s/%s%s.log", history_path, prefix, peer);
	if (rv == -1)
		return NULL;
	return path;
}

static struct history_file*
get_history_file(char *path) {
	struct history_file	*hf;

	LIST_FOREACH(hf, &history_files, hf_entry) {
		if (strcmp(hf->hf_path, path) == 0)
			goto found;
	}

	hf = calloc(1, sizeof(struct history_file));
	if (hf == NULL)
		goto fail;
	hf->hf_path = strdup(path);
	if (create_dir_for(hf->hf_path) == -1)
		goto fail;
	hf->hf_fd = -1;    /* to be opened later */
	SIMPLEQ_INIT(&hf->hf_tasks);
	LIST_INSERT_HEAD(&history_files, hf, hf_entry);

found:
	return hf;

fail:
	free(hf);
	return NULL;
}

/*
 * Creates directory recursively.
 * Given /foo/bar/buz as path, it'll attempt to create /foo/var directory.
 */
int
create_dir_for(char *path) {
	int	 ec;
	char	*slash;

	slash = strrchr(path, '/');
	if (slash == NULL) {
		errno = EINVAL;
		return -1;
	}
	*slash = '\0';
	ec = mkdir(path, 0777);
	if (ec != 0) {
		if (errno == EEXIST)
			ec = 0;
		else if (errno == ENOENT) {
			ec = create_dir_for(path);
			if (ec != -1)
				ec = mkdir(path, 0777);
		}
	}
	*slash = '/';
	return ec;
}

void
save_history(char type, const char *peer, const char *msg, int incoming) {
	struct history_file	*hf;
	struct icb_task		*it = NULL;
	struct tm		*now;
	size_t			 datasz;
	time_t			 t;
	char			*path;
	const int		 datelen = 20;

	if (!enable_history)
		return;

	t = time(NULL);
	now = localtime(&t);
	path = get_save_path_for(type, peer, msg);
	if (path == NULL)
		goto fail;
	hf = get_history_file(path);
	if (hf == NULL)
		goto fail;

	if (!incoming)
		peer = "me";
	datasz = datelen + strlen(peer) + 2 + strlen(msg) + 1;
	it = calloc(1, sizeof(struct icb_task) + datasz);
	if (it == NULL)
		goto fail;
	strftime(it->it_data, datasz, "%Y-%m-%d %H:%M:%S ", now);
	strlcat(it->it_data, peer, datasz);
	strlcat(it->it_data, ": ", datasz);
	strlcat(it->it_data, msg, datasz);
	it->it_len = datasz;
	it->it_data[datasz - 1] = '\n';
	SIMPLEQ_INSERT_TAIL(&hf->hf_tasks, it, it_entry);
	return;

fail:
	warn(__func__);
	free(path);
}

void
proceed_history(void) {
	struct history_file	*hf, *thf;
	struct icb_task	*it;
	ssize_t			 nwritten;

	LIST_FOREACH_SAFE(hf, &history_files, hf_entry, thf) {
		if (hf->hf_permerr)
			continue;
		if (hf->hf_fd == -1) {
			// error or reload happened
			hf->hf_fd = open(hf->hf_path,
			     O_WRONLY|O_CREAT|O_APPEND|O_NONBLOCK, 0666);
			if (hf->hf_fd == -1) {
				warnx("cannot open %s", hf->hf_path);
				while (!SIMPLEQ_EMPTY(&hf->hf_tasks)) {
					it = SIMPLEQ_FIRST(&hf->hf_tasks);
					SIMPLEQ_REMOVE_HEAD(&hf->hf_tasks, it_entry);
					free(it);
				}
				hf->hf_ntasks = 0;
				hf->hf_permerr = 1;
			}
		}
		while (!SIMPLEQ_EMPTY(&hf->hf_tasks)) {
			it = SIMPLEQ_FIRST(&hf->hf_tasks);
			do {
				nwritten = write(hf->hf_fd,
				    it->it_data + it->it_ndone,
				    it->it_len - it->it_ndone);
				if (nwritten == -1) {
					if (errno == EAGAIN)
						goto next_file;
					warn("cannit write history to %s",
					    hf->hf_path);
					close(hf->hf_fd);
					hf->hf_fd = -1;
					goto next_file;
				}
				it->it_ndone += nwritten;
			} while (it->it_ndone < it->it_len);
			SIMPLEQ_REMOVE_HEAD(&hf->hf_tasks, it_entry);
			free(it);
		}
		if (SIMPLEQ_EMPTY(&hf->hf_tasks) &&
		    hf->hf_last_access < time(NULL)) {
			LIST_REMOVE(hf, hf_entry);
			close(hf->hf_fd);
			free(hf->hf_path);
			free(hf);
		}
next_file:
		;
	}

}
