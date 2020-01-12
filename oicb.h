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

#ifndef OICB_OICB_H
#define OICB_OICB_H

#include <sys/queue.h>

#if LOGIN_NAME_MAX >64
# define NICKNAME_MAX 64
#else
# define NICKNAME_MAX LOGIN_NAME_MAX
#endif

SIMPLEQ_HEAD(icb_task_queue, icb_task);
struct icb_task {
	SIMPLEQ_ENTRY(icb_task)	it_entry;
	size_t	  it_len;
	size_t	  it_ndone;
	void	 *it_cb_data;
	void	(*it_cb)(struct icb_task *);
	char	  it_data[0];
};
extern struct icb_task_queue	tasks_stdout;

struct line_cmd {
	char	*start;	// same as the parse_cmd_line() argument
	char	*cmd_name;
	char	*cmd_name_end;
	int	 cmd_name_len;
	int	 has_args;
	int	 is_private;	// "/m" or "/msg"

	// only in private message commands
	int	 private_prefix_len;	// up to private_msg
	char	*peer_nick;
	char	*peer_nick_end;
	char	*private_msg;
	int	 peer_nick_offset;	// from start
	int	 peer_nick_len;
};

int	 parse_cmd_line(char *line, struct line_cmd *cmd);

void	 push_stdout_msg(const char *text);


extern int		 debug;

extern char	*room;

#endif // OICB_OICB_H
