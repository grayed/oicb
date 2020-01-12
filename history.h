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

#ifndef OICB_HISTORY_H
#define OICB_HISTORY_H


LIST_HEAD(history_files_list, history_file);
extern struct history_files_list history_files;
struct history_file {
	LIST_ENTRY(history_file)	hf_entry;
	struct icb_task_queue	hf_tasks;
	char	*hf_path;
	size_t	 hf_ntasks;
	int	 hf_fd;
	int	 hf_permerr;      // failed to open?
	time_t	 hf_last_access;
};

void	 save_history(char type, const char *peer, const char *msg,
	              int incoming);
void	 proceed_history(void);
int	 create_dir_for(char *path);

extern int		 enable_history;
extern char		 history_path[PATH_MAX];

#endif // OICB_HISTORY_H
