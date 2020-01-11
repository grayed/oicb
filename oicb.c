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
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>

#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <resolv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <vis.h>

#include <signal.h>

#include <readline/readline.h>

#if LOGIN_NAME_MAX >64
# define NICKNAME_MAX 64
#else
# define NICKNAME_MAX LOGIN_NAME_MAX
#endif

#ifndef HAVE_RL_BIND_KEYSEQ
static inline int	rl_bind_keyseq(const char *keyseq, int(*function)(int, int));
#endif


enum {
	Connecting,
	Connected,
	LoginSent,
	Chat,
	CommandSent,
} state = Connecting;

enum {
	Network = 0,
	Stdout,
	Stdin,
	MainFDCount
};
struct pollfd	 *pfd;
size_t		  npfd = 0;
static const char *stream_names[] = {
	"network",
	"stdout",
	"stdin",
};

SIMPLEQ_HEAD(icb_task_queue, icb_task) tasks_stdout, tasks_net;
struct icb_task {
	SIMPLEQ_ENTRY(icb_task)	it_entry;
	size_t	  it_len;
	size_t	  it_ndone;
	void	 *it_cb_data;
	void	(*it_cb)(struct icb_task *);
	char	  it_data[0];
};

LIST_HEAD(history_files_list, history_file) history_files;
struct history_file {
	LIST_ENTRY(history_file)	hf_entry;
	struct icb_task_queue	hf_tasks;
	char	*hf_path;
	size_t	 hf_ntasks;
	int	 hf_fd;
	int	 hf_permerr;      // failed to open?
	time_t	 hf_last_access;
};

int		 debug = 0;
int		 sock = -1, histfile = -1;
volatile int	 want_exit = 0;
volatile int	 want_info = 0;
char		*nick, *hostname, *room;
char		*o_rl_buf = NULL;
int		 o_rl_point, o_rl_mark;
int		 pings_sent = 0;
int		 last_cmd_has_nl = 0;
int		 enable_history = 1;
char		 history_path[PATH_MAX];

#define	PRIV_CHATS_MAX	5
char		 priv_chats_nicks[PRIV_CHATS_MAX][NICKNAME_MAX];
int		 priv_chats_cnt;
int		 repeat_priv_nick;
int		 prefer_long_priv_cmd;

enum SrvFeature {
	Ping	= 0x01,
	ExtPkt	= 0x02,
} srv_features = Ping;


void	 usage(const char *msg);
void	 pledge_me(void);
int	 test_cmd(int count, int key);

void	 push_stdout_msg(const char *text);
size_t	 push_data(int fd, char *data, size_t len);
void	 proceed_output(struct icb_task_queue *q, int fd);
void	 proceed_user_input(char *line);
void	 err_unexpected_msg(char type);
void	 err_invalid_msg(char type, const char *desc);
void	 proceed_icb_msg(char *msg, size_t len);
void	 push_icb_msg(char type, const char *src, size_t len);
void	 push_icb_msg_ws(char type, const char *src, size_t len);
void	 push_icb_msg_extended(char type, const char *src, size_t len);
char	*get_next_icb_msg(size_t *msglen);

void	 update_pollfds(void);
#ifdef SIGINFO
void	 siginfo_handler(int sig);
#endif
int	 siginfo_cmd(int count, int key);
void	 prepare_stdout(void);
void	 restore_rl(void);
void	 icb_connect(const char *addr, const char *port);

void	 proceed_chat_msg(char type, const char *author, const char *text);
void	 proceed_cmd_result(char *msg, size_t len);
void	 proceed_cmd_result_end(char *msg, size_t len);
void	 proceed_user_list(char *msg, size_t len);
void	 proceed_group_list(char *msg, size_t len);

char	*null_completer(const char *text, int cmpl_state);

struct history_file	*get_history_file(char *path);
char	*get_save_path_for(char type, const char *who);
void	 save_history(char type, const char *who, const char *msg);
void	 proceed_history(void);
int	 create_dir_for(char *path);

int	cmd_start(const char *line);
int	cmd_end(const char *line);
int	priv_cmd_end(const char *line);
int	priv_nick_start(const char *line);
int	priv_nick_end(const char *line);
int	priv_msg_start(const char *line);

void	update_nick_history(const char *cmdargs);
int	list_priv_chats_nicks(int foo, int bar);
int	cycle_priv_chats(int forward);

// readline wrappers
int	cycle_priv_chats_forward(int foo, int bar);
int	cycle_priv_chats_backward(int foo, int bar);


typedef void (*icb_msg_handler)(char *, size_t);
struct cmd_result_handler {
	char		outtype[4];
	icb_msg_handler	handler;
} cmd_handlers[] = {
	{ "co",	&proceed_cmd_result },
	{ "ec",	&proceed_cmd_result_end },
	{ "wl",	&proceed_user_list },
	{ "wg",	&proceed_group_list },

	// here deprecated/ignored ones go
	{ "wh",	NULL },     // show user list header
	{ "gh",	NULL },     // show group list header
	{ "ch",	NULL },     // show commands supported by client
	{ "c",	NULL }      // show one exact command description
};


#ifdef SIGINFO
void
siginfo_handler(int sig) {
	(void)sig;
	want_info = 1;
}
#endif

int
siginfo_cmd(int count, int key) {
	(void)count;
	(void)key;
	want_info = 1;
	return 0;
}

void
prepare_stdout(void) {
	char	*p;

	if (o_rl_buf != NULL)
		err(1, "internal error: already called prepare_stdout");
	o_rl_buf = strdup(rl_line_buffer);
	if (o_rl_buf == NULL)
		err(1, "prepare_stdout");
	o_rl_point = rl_point;
	o_rl_mark = rl_mark;
	for (p = rl_line_buffer; *p; p++)
		*p = ' ';
	rl_mark = 0;
	rl_point = 0;
	rl_redisplay();
}

void
restore_rl(void) {
	size_t	 len;

	if (repeat_priv_nick) {
		rl_clear_message();
		rl_point = 0;
		if (prefer_long_priv_cmd)
			rl_insert_text("/msg ");
		else
			rl_insert_text("/m ");
		rl_insert_text(priv_chats_nicks[0]);
		rl_insert_text(" ");
		kill(getpid(), SIGWINCH);
		repeat_priv_nick = 0;
	} else {
		len = strlen(o_rl_buf);
		rl_extend_line_buffer(len+2);
		(void) strlcpy(rl_line_buffer, o_rl_buf, len+1);
		rl_point = o_rl_point;
		rl_mark = o_rl_mark;
		rl_redisplay();
	}

	free(o_rl_buf);
	o_rl_buf = NULL;
}

/*
 * Queue text to be displayed.
 *
 * The data pointer won't be accessed after return, its contents will be
 * copied to internal buffer for the further processing.
 */
void
push_stdout_msg(const char *text) {
	struct icb_task	*it;
	size_t		 len;

	len = strlen(text) + 1;
	it = calloc(1, sizeof(struct icb_task) + len);
	if (it == NULL)
		err(1, "push_stdout_msg");
	it->it_len = len;
	memcpy(it->it_data, text, len);
	SIMPLEQ_INSERT_TAIL(&tasks_stdout, it, it_entry);
}

/*
 * Queue ICB messages to be sent to server.
 */
void
push_icb_msg(char type, const char *src, size_t len) {
	if (debug >= 2) {
		warnx("asked to push message of type '%c' with size %zu: "
		    "%s, queue %p", type, len, src, &tasks_net);
	}
	if ((srv_features & ExtPkt) == ExtPkt)
		push_icb_msg_extended(type, src, len);
	else
		push_icb_msg_ws(type, src, len);
}

/*
 * Split messages sent, preferrably on whitespace (for chat).
 * Send messages as separate packets, for compatibility's sake.
 */
void
push_icb_msg_ws(char type, const char *msg, size_t len) {
	struct icb_task	*it;
	int		 privmsg;
	unsigned char	 msglen, maxlen, commonlen;
	const char	*p, *src;

	commonlen = 0;
	privmsg = type == 'h' && memcmp(msg, "m\001", 2) == 0;
	if (privmsg) {
		p = strchr(msg, ' ');
		if (p != NULL && p - msg < NICKNAME_MAX + 3)
			commonlen = (unsigned char)(p - msg) + 1;
	}
	src = msg + commonlen;
	len -= commonlen;

	// give a chance to server to prepend nickname field without breaking
	maxlen = 253 - ((unsigned char)strlen(nick) + 1) - commonlen;
	do {
		if (len > maxlen) {
			msglen = maxlen;
			if (type == 'b' || privmsg)
				for (p = src + msglen - 1; p > src; p--)
					if (isblank(*p) || ispunct(*p)) {
						msglen = p - src + 1;
						break;
					}
		} else
			msglen = len;
		if ((it = calloc(1, sizeof(struct icb_task) + msglen + commonlen + 3)) == NULL)
			err(1, "push_icb_msg_ws");
		it->it_len = msglen + commonlen + 3;
		it->it_data[0] = (char)((unsigned char)msglen + commonlen + 2);
		it->it_data[1] = type;
		memcpy(it->it_data + 2, msg, commonlen);
		memcpy(it->it_data + 2 + commonlen, src, msglen);
		src += msglen;
		len -= msglen;
		SIMPLEQ_INSERT_TAIL(&tasks_net, it, it_entry);
	} while (len);
}

/*
 * Use proposed "extended" messages. Not tested on real servers yet.
 */
void
push_icb_msg_extended(char type, const char *src, size_t len) {
	struct icb_task	*it;
	size_t		 msgcnt;
	unsigned char	*dst, szfinal;

	len++;    // for trailing NUL
	msgcnt = (len + 253) / 254;
	if (debug >= 3) {
		warnx("there will be %zu messages", msgcnt);
	}

	// +1 for sizeof(struct icb_task)
	if ((it = calloc(msgcnt + 1, 256)) == NULL)
		err(1, "push_icb_msg_extended");
	it->it_len = len + msgcnt * 2;   // for size and type bytes in each message
	dst = (unsigned char *)it->it_data;
	while (msgcnt-- > 1) {
		*dst++ = 0;
		*dst++ = type;
		memcpy(dst, src, 254);
		src += 254;
		dst += 254;
		if (debug >= 3) {
			warnx("\tinitialized msg #%zu", msgcnt);
		}
	}
	szfinal = (unsigned char)(len % 254);
	if (debug >= 3) {
		warnx("\tputting last %hhu bytes", szfinal);
	}
	*dst++ = szfinal + 1;    // for type byte
	*dst++ = type;
	memcpy(dst, src, szfinal);
	// NUL is already there, thanks to calloc
	SIMPLEQ_INSERT_TAIL(&tasks_net, it, it_entry);
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

char*
get_save_path_for(char type, const char *who) {
	int		 rv;
	const char	*prefix;
	char		*path;

	if (type != 'c') {
		who = room;
		prefix = "room-";
	} else {
		prefix = "private-";
	}

	rv = asprintf(&path, "%s/%s%s.log", history_path, prefix, who);
	if (rv == -1)
		return NULL;
	return path;
}

struct history_file*
get_history_file(char *path) {
	struct history_file	*hf;

	LIST_FOREACH(hf, &history_files, hf_entry) {
		if (strcmp(hf->hf_path, path) == 0)
			goto found;
	}

	hf = calloc(1, sizeof(struct history_file));
	if (hf == NULL)
		goto err;
	hf->hf_path = strdup(path);
	if (create_dir_for(hf->hf_path) == -1)
		goto err;
	hf->hf_fd = -1;    /* to be opened later */
	SIMPLEQ_INIT(&hf->hf_tasks);
	LIST_INSERT_HEAD(&history_files, hf, hf_entry);

found:
	return hf;

err:
	free(hf);
	return NULL;
}

// Input: command line, starting with /m or /msg.
// Returns: index where command name starts, or 0 if no command name
int
cmd_start(const char *line) {
	const char	*p = line;

	if (*p++ != '/')
		return 0;
	while (*p != '\0' && isspace(*p))
		p++;
	if (!isalpha(*p))
		return 0;
	return (int)(p - line);
}

// Input: command line, starting with /m or /msg.
// Returns: index of first char after command name, or 0 if no command name
int
cmd_end(const char *line) {
	size_t		 idx;

	if ((idx = cmd_start(line)) == 0)
		return 0;
	while (isalpha(line[idx]))
		idx++;
	return idx;
}

// Input: command line, starting with /m or /msg.
// Returns: index where command name ends, or 0 if not /m or /msg.
int
priv_cmd_end(const char *line) {
	size_t		 idx;
	const char	*p;

	if ((idx = cmd_start(line)) == 0)
		return 0;
	p = line + idx;
	if (*p++ != 'm')
		return 0;
	if (p[0] == 's' && p[1] == 'g')
		p += 2;
	if (*p != '\0' && !isspace(*p))
		return 0;
	return (int)(p - line);
}

// Input: command line, starting with /m or /msg.
// Return: index of nickname, or 0 if not /m or /msg, or no nickname.
int
priv_nick_start(const char *line) {
	size_t	idx;

	if ((idx = priv_cmd_end(line)) == 0)
		return 0;
	while (isspace(line[idx]))
		idx++;
	if (line[idx] == '\0')
		return 0;
	return idx;
}

// Input: command line, starting with /m or /msg.
// Return: index of first char after nickname, or 0 if not /m or /msg, or no nickname.
int
priv_nick_end(const char *line) {
	size_t	idx;

	if ((idx = priv_nick_start(line)) == 0)
		return 0;
	while (line[idx] != '\0' && !isspace(line[idx]))
		idx++;
	return idx;
}

// Input: command line, starting with /m or /msg.
// Return: index of message text, or 0 if not /m or /msg, or no message.
int
priv_msg_start(const char *line) {
	size_t	idx;

	if ((idx = priv_nick_end(line)) == 0)
		return 0;
	if (!isspace(line[idx]))
		return 0;
	return idx + 1;
}

// Input: private message command args, or NULL for public message.
void
update_nick_history(const char *cmdargs) {
	if (debug >= 2)
		warnx("%s: cmdargs='%s'", __func__, cmdargs);
	if (cmdargs != NULL) {
		size_t	nicklen;
		int	i;
		char	tmp[NICKNAME_MAX];

		while (isspace(*cmdargs))
			cmdargs++;
		for (i = 0; cmdargs[i] != '\0' && !isspace(cmdargs[i]); i++)
			;
		if (i >= NICKNAME_MAX) {
			push_stdout_msg(getprogname());
			push_stdout_msg(": warning: nickname is too long\n");
			return;
		}
		nicklen = (size_t)i;

		if (priv_chats_cnt > 0 &&
		    strncmp(priv_chats_nicks[0], cmdargs, nicklen) == 0 &&
		    priv_chats_nicks[0][nicklen] == '\0')
			return;		// already topmost one
		for (i = 1; i < priv_chats_cnt; i++) {
			if (strncmp(priv_chats_nicks[i], cmdargs, nicklen) == 0) {
				if (debug >=2)
					warnx("%s: found %s", __func__, priv_chats_nicks[i]);
				// make current nick the newest one
				memcpy(tmp, priv_chats_nicks[i], NICKNAME_MAX);
				memmove(priv_chats_nicks[1], priv_chats_nicks[0], i * NICKNAME_MAX);
				memcpy(priv_chats_nicks[0], tmp, NICKNAME_MAX);
				return;
			}
		}

		if (debug >= 2)
			warnx("%s: copying first %d bytes from '%s' to %p", __func__, (int)nicklen, cmdargs, priv_chats_nicks[0]);

		// add nick to history, possibly kicking out oldest one
		memmove(priv_chats_nicks[1], priv_chats_nicks[0],
		    (PRIV_CHATS_MAX - 1) * NICKNAME_MAX);
		memcpy(priv_chats_nicks[0], cmdargs, nicklen);
		memset(priv_chats_nicks[0] + nicklen, 0, NICKNAME_MAX - nicklen);
		if (priv_chats_cnt < PRIV_CHATS_MAX - 1)
			priv_chats_cnt++;
	}
}

//
// The logic as follows:
//
//  1. If private chat nicknames history is empty, just beep.
//
//  2. If this is a public chat message, switch to last used private chat.
//
//  3. If this is not either public or private chat line, just beep.
//
//  4. If this is a private chat command without nickname, switch to last
//      used private chat.
//
//  5. If the private chat nickname is the oldest used in nick history,
//     clear private command, making the message public.
//
//  6. If private chat nickname is not remembered, but there is remembered
//     nickname which starts like this, fill the latter nickname fully.
//
//  7. If there is no matching private chat nickname in history, switch to the
//     last used private chat.
//
//  8. Now this is a non-oldest remembered private chat nickname: replace
//     nickname in private commmand with older one.
//
int
cycle_priv_chats(int forward) {
	size_t		nicklen;	// length of entered nickname
	int		i;
	int		newidx;		// index from history to use
	int		oldcurpos;	// initial rl_point value
	int		nickpos;	// offset of nick in entered line
	const char	*buf;		// because rl_line_buffer name is too long

	if (debug >= 2)
		warnx("%s: forward=%d\n", __func__, forward);

	if (priv_chats_cnt == 0) {
		// (1)
		putchar('\007');
		return 0;
	}

	oldcurpos = rl_point;
	buf = rl_line_buffer;
	// Rememeber that after calling rl_*_text() the value of
	// rl_line_buffer may change.

	if (buf[0] == '/') {
		int	pce;

		// (3), (4), (5), (6), (7) or (8)
		if (debug >= 2)
			warnx("%s: 3-4-5-6-7-8 buf='%s'", __func__, buf);

		pce = priv_cmd_end(buf);
		if (!pce) {
			// (3)
			putchar('\007');
			return 0;
		}
		prefer_long_priv_cmd = buf[pce-1] == 'g';

		nickpos = priv_nick_start(buf);
		if (nickpos > 0) {
			const char	*priv_nick = buf + nickpos;

			// (5), (6), (7) or (8)
			if (debug >= 2)
				warnx("%s: 5-6-7-8 nickpos=%d", __func__, nickpos);

			for (i = 0; priv_nick[i] != '\0' && !isspace(priv_nick[i]); i++)
				;
			nicklen = (size_t)i;
			for (i = 0; i < priv_chats_cnt; i++)
				if (nicklen == strlen(priv_chats_nicks[i]) &&
				   memcmp(priv_nick, priv_chats_nicks[i], nicklen) == 0)
					break;
			if (( forward && i == priv_chats_cnt - 1) ||
			    (!forward && i == 0)) {
				// (5)
				// "+ 1" for space after nick; if there is none,
				// readline will handle it fine.
				rl_delete_text(0, nickpos + (int)nicklen + 1);
				rl_point -= nickpos + (int)nicklen + 1;
				if (rl_point < 0)
					rl_point = 0;
				return 0;
			}

			if (i == priv_chats_cnt) {
				// (6) or (7): do prefix matching check
				if (forward) {
					for (i = 0; i < priv_chats_cnt; i++)
						if (strncmp(priv_nick, priv_chats_nicks[i], nicklen) == 0) {
							// (6)
							newidx = i;
							goto replace_nick;
						}
				} else {
					for (i = priv_chats_cnt - 1; i >= 0; i--)
						if (strncmp(priv_nick, priv_chats_nicks[i], nicklen) == 0) {
							// (6)
							newidx = i;
							goto replace_nick;
						}
				}

				// (7)
				newidx = forward ? 0 :  priv_chats_cnt - 1;
				goto replace_nick;
			}

			// (8)
			newidx = i + (forward ? 1 : -1);
			goto replace_nick;
		}

		// (4)
		nicklen = 0;
		newidx = 0;
	} else {
		// (2)
		rl_point = 0;
		if (prefer_long_priv_cmd) {
			rl_insert_text("/msg ");
			nickpos = 5;
		} else {
			rl_insert_text("/m ");
			nickpos = 3;
		}
		nicklen = 0;
		newidx = forward ? 0 : priv_chats_cnt - 1;
		oldcurpos += nickpos + 1;	// "+ 1" for space
	}

	// (2), (4), (6), (7) or (8)

replace_nick:
	if (debug >= 2) {
		warnx("%s: replace_nick rl_line_buffer='%s' rl_point=%d oldcurpos=%d nickpos=%d nicklen=%zu",
		    __func__, rl_line_buffer, rl_point, oldcurpos, nickpos, nicklen);
	}

	if (nicklen)
		rl_delete_text(nickpos, nickpos + (int)nicklen + 1);
	rl_point = nickpos;
	rl_insert_text(priv_chats_nicks[newidx]);
	rl_insert_text(" ");

	// restoring cursor position
	if (oldcurpos < nickpos)
		rl_point = oldcurpos;
	else if (oldcurpos > nickpos + (int)nicklen)
		rl_point = oldcurpos
		    + (int)strlen(priv_chats_nicks[newidx])
		    - (int)nicklen;
	else // cursors was inside nickname, play safe
		rl_point = nickpos + nicklen;
	return 0;
}

int
cycle_priv_chats_forward(int foo, int bar) {
	(void)foo;
	(void)bar;
	cycle_priv_chats(1);
	return 0;
}

int
cycle_priv_chats_backward(int foo, int bar) {
	(void)foo;
	(void)bar;
	cycle_priv_chats(0);
	return 0;
}

int
list_priv_chats_nicks(int foo, int bar) {
	int	i;
	char	buf[100];

	(void)foo;
	(void)bar;

	if (priv_chats_cnt == 0) {
		push_stdout_msg("there are no private chats yet\n");
		return 0;
	}

	snprintf(buf, sizeof(buf), "there are %d private chats:\n",
	    priv_chats_cnt);
	push_stdout_msg(buf);
	for (i = 0; i < priv_chats_cnt; i++) {
		push_stdout_msg("  * ");
		push_stdout_msg(priv_chats_nicks[i]);
		push_stdout_msg("\n");
	}
	return 0;
}

void	
save_history(char type, const char *who, const char *msg) {
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
	path = get_save_path_for(type, who);
	if (path == NULL)
		goto err;
	hf = get_history_file(path);
	if (hf == NULL)
		goto err;

	datasz = datelen + strlen(who) + 2 + strlen(msg) + 1;
	it = calloc(1, sizeof(struct icb_task) + datasz);
	if (it == NULL)
		goto err;
	strftime(it->it_data, datasz, "%Y-%m-%d %H:%M:%S ", now);
	strlcat(it->it_data, who, datasz);
	strlcat(it->it_data, ": ", datasz);
	strlcat(it->it_data, msg, datasz);
	it->it_len = datasz;
	it->it_data[datasz - 1] = '\n';
	SIMPLEQ_INSERT_TAIL(&hf->hf_tasks, it, it_entry);
	return;

err:
	warn("save_history");
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
				warnx("can't open %s", hf->hf_path);
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
				nwritten = write(hf->hf_fd, it->it_data + it->it_ndone,
				    it->it_len - it->it_ndone);
				if (nwritten == -1) {
					if (errno == EAGAIN)
						goto next_file;
					warn("can't write history to %s", hf->hf_path);
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
			free(hf->hf_path);
			free(hf);
		}
next_file:
		;
	}

}

size_t
push_data(int fd, char *data, size_t len) {
	ssize_t	nwritten = 0;
	size_t	total = 0;

	while (len > 0 && (nwritten = write(fd, data, len)) >= 0) {
		len -= (size_t) nwritten;
		data += nwritten;
		total += nwritten;
	}
	if (nwritten != -1 || errno == EAGAIN)
		return total;
	err(2, "write");
}

/*
 * Push queued text.
 */
void
proceed_output(struct icb_task_queue *q, int fd) {
	struct icb_task	*it;
	size_t		 nwritten;

	while (!SIMPLEQ_EMPTY(q)) {
		it = SIMPLEQ_FIRST(q);
		nwritten = push_data(fd, it->it_data + it->it_ndone,
			it->it_len - it->it_ndone);
		if (debug >= 2) {
			warnx("output %zu from %zu bytes at fileno %d",
			    it->it_ndone, it->it_len, fd);
		}
		it->it_ndone += nwritten;
		if (it->it_ndone < it->it_len)
			break;
		SIMPLEQ_REMOVE_HEAD(q, it_entry);
		if (it->it_cb)
			(*it->it_cb)(it);
		free(it);
	}
}

/*
 * Handle text line coming from libreadline.
 */
void
proceed_user_input(char *line) {
	char	*p;
	size_t	 n;

	if (line == NULL) {
		want_exit = 1;
		return;
	}
	for (p = line; isspace(*p); p++)
		;
	if (!*p)
		// add some insult like icb(1) does?
		return;
	if (line[0] == '/' && line[1] && !isspace(line[1])) {
		int	c_end, pc_end, nick_start, pms;

		c_end = cmd_end(line);
		pc_end = priv_cmd_end(line);
		nick_start = priv_nick_start(line);
		pms = priv_msg_start(line);
		if (debug >= 2)
			warnx("%s: c_end=%d pc_end=%d nick_start=%d pms=%d line='%s'",
			    __func__, c_end, pc_end, nick_start, pms, line);

		if (line[c_end])
			line[c_end] = '\001';    // separate args

		if (pc_end > 0) {
			if (pms == 0)
				return;    // skip empty line
			update_nick_history(&line[nick_start]);
			save_history('c', "me", &line[pms]);
			repeat_priv_nick = 1;
			prefer_long_priv_cmd = line[pc_end-1] == 'g';
		}
		push_icb_msg('h', line + 1, strlen(line) - 1);
		state = CommandSent;
		return;
	}
	// public message
	update_nick_history(NULL);
	save_history('b', "me", line);
	push_icb_msg('b', line, strlen(line));
}

__dead void
err_unexpected_msg(char type) {
	err(2, "unexpected message of type '%c' received", type);
}

__dead void
err_invalid_msg(char type, const char *desc) {
	err(2, "invalid message of type '%c' received: %s", type, desc);
}

/*
 * Queue formatted incoming chat message for displaying.
 */
void
proceed_chat_msg(char type, const char *author, const char *text) {
	size_t		 szbuf, curlen;
	char		*buf;
	const char	*preuser, *postuser;
	time_t		 t;

	save_history(type, author, text);

	switch (type) {
	case 'c':
		preuser  = " *";
		postuser = "* ";
		break;
	case 'd':
		preuser  = " [=";
		postuser = "=] ";
		break;
	case 'e':
	case 'k':
		preuser  = " !";
		postuser = "! ";
		break;
	case 'f':
		preuser  = " {";
		postuser = "} ";
		break;
	default:
		preuser  = " <";
		postuser = "> ";
	}

	szbuf = sizeof("[00:00:00]\n") + strlen(preuser) + strlen(author)*4 +
	    strlen(postuser) + strlen(text)*4;

	if ((buf = malloc(szbuf)) == NULL)
		err(1, "proceed_chat_msg");

	t = time(NULL);
	strftime(buf, szbuf, "[%H:%M:%S]", localtime(&t));
	curlen = strlcat(buf, preuser, szbuf);
	strvis(buf + curlen, author, VIS_SAFE|VIS_NOSLASH);
	curlen = strlcat(buf, postuser, szbuf);
	strvis(buf + curlen, text, VIS_SAFE|VIS_NOSLASH);
	if (strlcat(buf, "\n", szbuf) >= szbuf)
		errx(1, "proceed_chat_msg: internal error");
	push_stdout_msg(buf);
	free(buf);
}

void
proceed_cmd_result(char *msg, size_t len) {
	size_t	 bufsz;
	char	*buf;

	bufsz = len * 4 + 1;
	if ((buf = malloc(bufsz)) == NULL)
		err(1, "proceed_cmd_result");
	strvisx(buf, msg, len, VIS_SAFE|VIS_NOSLASH);
	last_cmd_has_nl = (msg[len-1] == '\n');
	push_stdout_msg(buf);
	free(buf);
}

void
proceed_cmd_result_end(char *msg, size_t len) {
	(void)msg;
	(void)len;

	if (last_cmd_has_nl)
		last_cmd_has_nl = 0;
	else
		push_stdout_msg("\n");
	state = Chat;
}

void
proceed_user_list(char *msg, size_t len) {
	char		*p, *endptr;
	const char	*peer_nick, *ident, *srcaddr;
	int		 nicklen;
	long long	 signedon, idle;

	(void)len;

/*
moderator ("m" or else)
nickname
idle time
0 (always zero)
signon timestamp (ex.: 1460893072)
user (ident result)
IP address/domain
*/

	p = strchr(msg, '\001');
	if (p == NULL) {
		warnx("invalid user info line received, ignoring");
		return;
	}
	*p = '\0';
	if (p == msg + 1 && *msg == 'm')
		push_stdout_msg("*");
	else
		push_stdout_msg(" ");
	peer_nick = p + 1;
	p = strchr(peer_nick, '\001');
	if (p != NULL) {
		*p = '\0';
		nicklen = p - peer_nick;
	} else {
		nicklen = strlen(peer_nick);
	}
	(void)nicklen;	// TODO
	push_stdout_msg(peer_nick);
	if (p == NULL)
		goto end;
	p++;
	idle = strtoll(p, &endptr, 10);
	(void)idle;	// TODO
	*endptr = '\0';
	push_stdout_msg(p);
	push_stdout_msg("s");
	p = strchr(endptr + 1, '\001');
	if (p == NULL)
		goto end;
	/* this field is always zero, no interest */
	p++;
	signedon = strtoll(p, &endptr, 10);
	if (*endptr != '\001' && *endptr != '\0')
		goto end;
	push_stdout_msg(ctime((time_t*)&signedon));

	ident = endptr + 1;
	p = strchr(ident, '\001');
	if (p != NULL)
		*p = '\0';
	push_stdout_msg(ident);
	if (p == NULL)
		goto end;

	srcaddr = p + 1;
	p = strchr(ident, '\001');
	if (p != NULL)
		*p = '\0';
	push_stdout_msg(srcaddr);

end:
	push_stdout_msg("\n");
}

void
proceed_group_list(char *msg, size_t len) {
	size_t		 bufsz, namelen, topiclen;
	char		*buf, *name, *topic, *msgid;
	const size_t	 min_name_len = 30;

	(void)len;

	name = msg;
	if ((topic = strchr(name, '\001')) == NULL) {
		warnx("invalid group info line received, ignoring");
		return;
	}
	*topic++ = '\0';
	if ((msgid = strchr(topic, '\001')) != NULL)
		*msgid++ = '\0';

	namelen = strlen(name) + 1;    // 1 for current-group marker
	topiclen = strlen(topic);
	bufsz = ((namelen < topiclen) ? topiclen : namelen) * 4;
	if (bufsz < min_name_len)
		bufsz = min_name_len;
	bufsz += 3;    // for marker, NUL and delimiter (space or \n)
	if ((buf = malloc(bufsz)) == NULL)
		err(1, "proceed_group_list");
	if (strcmp(name, room) == 0)
		buf[0] = '*';
	else
		buf[0] = ' ';
	strvis(buf + 1, name, VIS_SAFE|VIS_NOSLASH|VIS_NL);
	namelen = strlen(buf);
	for (; namelen <= min_name_len; namelen++)
		(void) strlcat(buf, " ", bufsz);
	push_stdout_msg(buf);
	strvis(buf, topic, VIS_SAFE|VIS_NOSLASH|VIS_NL);
	(void) strlcat(buf, "\n", bufsz);
	push_stdout_msg(buf);
	free(buf);
}

/*
 * Handle reconstructed incoming ICB message.
 */
void
proceed_icb_msg(char *msg, size_t len) {
	char	type;

	type = *msg++;
	len--;
	if (debug) {
		warnx("got message of type %c with size %zu: %s",
		    type, len, msg);
	}
	switch (type) {
	case 'a':	// login okay
		if (state != LoginSent)
			err_unexpected_msg(type);
		push_stdout_msg("Logged in to room ");
		push_stdout_msg(room);
		push_stdout_msg(" as ");
		push_stdout_msg(nick);
		push_stdout_msg("\n");
		state = Chat;
		break;

	case 'b':	// open message
	case 'c':	// private message
	case 'd':	// status message
	case 'f':	// important message
	{
		char	*text;
		if (state == CommandSent)
			state = Chat;
		else if (state != Chat)
			err_unexpected_msg(type);
		if ((text = strchr(msg, '\001')) == NULL)
			err_invalid_msg(type, "missing text");
		*text++ = '\0';
		proceed_chat_msg(type, msg, text);
		break;
	}

	case 'e':	// error
		if (state != Chat && state != CommandSent)
			want_exit = 1;
		if (strcmp(msg, "Undefined message type 108") == 0) {
			/* server doesn't support ping-pong */
			srv_features &= (~Ping);
			/* XXX set socket timeout options? */
			if (debug) {
				warnx("server doesn't support ping-pong,"
				    " switching to no-op messages");
			}
			break;
		}
		proceed_chat_msg(type, hostname, msg);
		break;

	case 'g':       // exit
		if (state != Chat)
			err_unexpected_msg(type);
		push_stdout_msg("ICB: server said bye-bye\n");
		want_exit = 1;
		break;

	case 'i':       // command result
	{
		int	 i;
		char	*outtype;

		if (state != CommandSent)
			err_unexpected_msg(type);
		outtype = msg;
		if ((msg = strchr(msg, '\001')) == NULL)
			err_invalid_msg(type, "missing output type");
		*msg++ = '\0';
		len -= msg - outtype;
		for (i = 0; i < (int)(sizeof(cmd_handlers)/sizeof(cmd_handlers[0]));
		    i++)
			if (strcmp(outtype, cmd_handlers[i].outtype) == 0) {
				if (cmd_handlers[i].handler)
					cmd_handlers[i].handler(msg, len);
				goto cmd_handler_found;
			}
		err_invalid_msg(type, "unsupported output type");
cmd_handler_found:
		break;
	}

	case 'j':       // protocol
	{
		char	*hostid = "HIDDEN", *srvid = "unknown implementation", *p;

		if (state != Connected)
			err_unexpected_msg(type);
		if ((p = strchr(msg, '\001')) != NULL) {
			*p++ = '\0';
			hostid = p;
			if ((p = strchr(hostid, '\001')) != NULL) {
				*p++ = '\0';
				srvid = p;
			}
		}
		// TODO use srvid
		(void)srvid;
		if (strcmp(msg, "1") != 0)
			err(2, "unsupported protocol version");
		if (asprintf(&p, "%1$s\001%1$s\001%2$s\001login\001", nick, room) == -1)
			err(1, "proceed_icb_message");
		push_icb_msg('a', p, strlen(p));
		free(p);
		state = LoginSent;
		break;
	}

	case 'k':       // beep
		if (state != Chat)
			err_unexpected_msg(type);
		proceed_chat_msg(type, "SERVER", "BEEP!");
		break;

	case 'l':       // ping
		push_icb_msg('m', msg, len);
		break;

	case 'm':       // pong
		/*
		 * XXX silently ignoring other unexpected pongs,
		 * even if server said it doesn't support them previously.
		 *
		 * The main purpose of pings sent are forcing server to send
		 * something back, so we don't bother with message IDs.
		 */
		break;

	case 'n':       // no-op
		if (state != Chat)
			err_unexpected_msg(type);
		break;

	default:
		{
			char nmsg[] =
			    "unsupported message of type 'X', ignored\n";
			/* yes, I'm lazy */
			*strchr(nmsg, 'X') = type;
			push_stdout_msg(nmsg);
		}
	}
}

/*
 * Extract next incoming ICB message on the network socket.
 *
 * Returned pointer contains message type in the first byte,
 * with data bytes following it. Data always ends with NUL,
 * which isn't taken into account of msglen returned.
 *
 * Returned pointer will be valid until next call of get_next_icb_msg().
 */
char*
get_next_icb_msg(size_t *msglen) {
	static unsigned char	*buf = NULL, *msgend = NULL;
	static size_t		 bufread = 0, bufsize = 1024;

	unsigned char	*lastpkt, *pkt, *nbuf;
	size_t		 roundread = 0;
	ssize_t		 nread, shift;

	if (buf == NULL) {
		if ((buf = malloc(bufsize)) == NULL)
			err(1, "get_next_icb_msg");

	} else if (msgend) {
		// resetting state
		memmove(buf, msgend, bufread - (msgend - buf));
		bufread -= (msgend - buf);
		msgend = NULL;
	}

	for (;;) {
		// Reserve one byte for ending NUL in case it's missing in the
		// input packet, see the "msglen++" block at the end of function.
		if (bufread == bufsize - 1) {
			if (bufsize >= 1024*1024)
				err(2, "too long message");
			if ((nbuf = reallocarray(buf, 2, bufsize)) == NULL)
				err(1, "get_next_icb_msg");
			buf = nbuf;
			bufsize *= 2;
		}
		nread = read(sock, buf + bufread, bufsize - bufread - 1);
		if (nread < 0) {
			if (errno != EAGAIN)
				err(1, "get_next_icb_msg");
			break;
		} else if (nread == 0) {
			push_stdout_msg("Server ");
			push_stdout_msg(hostname);
			push_stdout_msg(" closed connection, exiting...\n");
			want_exit = 1;
			break;
		}
		roundread += nread;
		bufread += nread;
	}
	if (bufread == 0 && roundread == 0)
		return NULL;

	for (lastpkt = buf; lastpkt[0] == 0; lastpkt += 256) {
		if (buf + bufread - 256 < lastpkt)
			return NULL;    // not received ending packet yet
	}
	// now lastpkt points to the ending packet
	if (buf + bufread - lastpkt[0] < lastpkt)
		return NULL;    // not received ending packet fully yet

	msgend = lastpkt + 1 + lastpkt[0];

	// got full message, now remove extra data to get continious bytes
	for (pkt = buf; pkt <= lastpkt; pkt += 256) {
		if (pkt[1] != lastpkt[1])
			// XXX Or just ignore? Which to use then?
			err(2, "message types messed up in a single message");
		if (pkt != buf) {
			shift = (pkt[-1] == '\0') ? 3 : 2;
			memmove(pkt + 2 - shift, pkt + 2, buf + bufread - (pkt + 2));
			bufread -= shift;
			lastpkt -= shift;
			pkt -= shift;
		}
	}

	// There always will be a place for NUL, see main read loop.
	if (msgend[-1] != '\0') {
		memmove(msgend + 1, msgend, bufread - (msgend - buf));
		*msgend++ = '\0';
	}

	// -1 to skip initial byte count byte, another -1 for trailing NUL
	*msglen = msgend - buf - 2;
	return (char*)(buf + 1);
}

char *
null_completer(const char *text, int cmpl_state) {
	(void)text;
	(void)cmpl_state;
	return NULL;
}

__dead void
usage(const char *msg) {
	if (msg)
		fprintf(stderr, "%s\n", msg);
	fprintf(stderr, "usage: %s [-dH] [-t secs] [nick@]host[:port] room\n",
	    getprogname());
	exit (1);
}

void
update_pollfds(void) {
	const struct history_file	*hfile;
	size_t				 newnpfd;
	int				 i;

	newnpfd = MainFDCount;
	LIST_FOREACH(hfile, &history_files, hf_entry)
		newnpfd++;
	if (npfd < newnpfd) {
		pfd = reallocarray(pfd, newnpfd, sizeof(struct pollfd));
		if (pfd == NULL)
			err(1, "update_pollfds");
		npfd = newnpfd;
	}

	memset(pfd, 0, sizeof(struct pollfd) * npfd);

	pfd[Stdin].fd = STDIN_FILENO;
	pfd[Stdin].events = (state == Connecting) ? 0 : POLLIN;

	pfd[Stdout].fd = STDOUT_FILENO;
	pfd[Stdout].events = 0;
	if (!SIMPLEQ_EMPTY(&tasks_stdout))
		pfd[Stdout].events |= POLLOUT;

	pfd[Network].fd = sock;
	pfd[Network].events = POLLIN;
	if (!SIMPLEQ_EMPTY(&tasks_net))
		pfd[Network].events |= POLLOUT;

	i = 0;
	LIST_FOREACH(hfile, &history_files, hf_entry) {
		pfd[MainFDCount + i].fd = hfile->hf_fd;
		if (hfile->hf_ntasks)
			pfd[MainFDCount + i].events = POLLOUT;
		i++;
	}
}

void
icb_connect(const char *addr, const char *port) {
	struct addrinfo		*res, *p, hints;
	int			 ec;

	if (port == NULL)
		port = "7326";
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if ((ec = getaddrinfo(addr, port, &hints, &res)) != 0)
		errx(1, "could not resolve host/port name: %s", gai_strerror(ec));
	for (p = res; p != NULL; p = p->ai_next) {
		if ((sock = socket(p->ai_family, p->ai_socktype|SOCK_NONBLOCK,
		    p->ai_protocol)) == -1) {
			warn("could not create socket");
			continue;
		}
		if (connect(sock, (struct sockaddr *)p->ai_addr,
		    p->ai_addrlen) == -1) {
			if (errno != EINPROGRESS) {
				/* make sure we'll report connect()'s error */
				ec = errno;
				close(sock);
				errno = ec;
				continue;
			}
			state = Connecting;
			push_stdout_msg("Connecting to ");
			push_stdout_msg(hostname);
			push_stdout_msg("... ");
		} else {
			state = Connected;
			push_stdout_msg("Connected to ");
			push_stdout_msg(hostname);
			push_stdout_msg("\n");
		}
		freeaddrinfo(res);
		return;
	}
	freeaddrinfo(res);
	err(1, "could not connect");
}

void
pledge_me() {
#ifdef HAVE_UNVEIL

	if (enable_history) {
		if (unveil(history_path, "rwc") == -1)
			err(1, "unveil");
	}
	if (unveil(NULL, NULL) == -1)
		err(1, "unveil");
#endif

#ifdef HAVE_PLEDGE
	if (pledge("stdio wpath cpath tty", NULL) == -1)
		err(1, "pledge");
#endif
}

#ifndef HAVE_RL_BIND_KEYSEQ
static inline int
rl_bind_keyseq(const char *keyseq, int(*function)(int, int))
{
	return rl_generic_bind(ISFUNC, keyseq, (char *)function, rl_get_keymap());
}
#endif

int
main(int argc, char **argv) {
#ifdef SIGINFO
	struct sigaction sa;
#endif
	size_t		 msglen;
	time_t		 ts_lastnetinput, t;
	int		 ch, i, net_timeout, poll_timeout, max_pings;
	char		*msg, *port = NULL;
	const char	*errstr;

	SIMPLEQ_INIT(&tasks_stdout);
	SIMPLEQ_INIT(&tasks_net);

	net_timeout = 30;
	while ((ch = getopt(argc, argv, "dHt:")) != -1) {
		switch (ch) {
		case 'd':
			debug++;
			break;
		case 'H':
			enable_history = 0;
			break;
		case 't':
			net_timeout = strtonum(optarg, 0, INT_MAX/1000,
			    &errstr);
			if (errstr)
				err(1, "network timeout specified is %s",
				    errstr);
			break;
		default:
			/* error message is already printed by getopt() */
			usage(NULL);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc != 2)
		usage(NULL);

	room = argv[1];

	if ((hostname = strchr(argv[0], '@')) != NULL) {
		nick = argv[0];
		*hostname++ = '\0';
		if (*hostname == '\0')
			usage("invalid hostname specification");
	} else {
		hostname = argv[0];
		nick = getlogin();
	}
	if (strlen(nick) >= NICKNAME_MAX)
		usage("too long nickname");

	if (hostname[0] == '[') {
		char	*hostend;

		hostname++;
		hostend = strrchr(hostname, ']');
		if (hostend == NULL ||
		    (hostend[1] != '\0' && hostend[1] != ':'))
			usage("invalid hostname specification");
		if (hostend[1] == ':')
			port = hostend + 2;
		*hostend = '\0';
	} else {
		if ((port = strrchr(hostname, ':')) != NULL)
			*port++ = '\0';
	}

	icb_connect(hostname, port);
	if (fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) == -1)
		err(1, "stdin: fcntl");
	if (fcntl(STDOUT_FILENO, F_SETFL, O_NONBLOCK) == -1)
		err(1, "stdout: fcntl");

	if (net_timeout)
		poll_timeout = net_timeout * 100;
	else
		poll_timeout = INFTIM;
	ts_lastnetinput = time(NULL);
	max_pings = 3;

	rl_callback_handler_install("", &proceed_user_input);
	atexit(&rl_callback_handler_remove);

	// disable completion, or readline will try to access file system
	rl_completion_entry_function = null_completer;

	rl_bind_key('\t', cycle_priv_chats_forward);
	rl_bind_keyseq("\\e[Z", cycle_priv_chats_backward);
	rl_bind_key(CTRL('p'), list_priv_chats_nicks);
	rl_bind_key(CTRL('t'), siginfo_cmd);
	if (debug)
		rl_bind_key(CTRL('x'), test_cmd);

#ifdef SIGINFO
	if (sigaction(SIGINFO, NULL, &sa) == -1)
		err(1, "sigaction(SIGINFO, NULL)");
	sa.sa_handler = siginfo_handler;
	if (sigaction(SIGINFO, &sa, NULL) == -1)
		err(1, "sigaction(SIGINFO, &sa)");
#endif

	if (enable_history) {
		snprintf(history_path, PATH_MAX, "%s/.oicb/logs/%s",
		    getenv("HOME"), hostname);
		if (create_dir_for(history_path) == -1 ||
		    (mkdir(history_path, 0777) == -1 && errno != EEXIST)) {
			warn("cannot make sure history directory \"%s\" exists",
			    history_path);
			warnx("history saving is disabled");
			enable_history = 0;
			memset(history_path, 0, PATH_MAX);
		}
	}

	pledge_me();

	while (!want_exit) {
		if (want_info) {
			push_stdout_msg(getprogname());
			push_stdout_msg(": ");
			push_stdout_msg("sitting in room ");
			push_stdout_msg(room);
			push_stdout_msg(" at ");
			push_stdout_msg(hostname);
			if (port) {
				push_stdout_msg(":");
				push_stdout_msg(port);
			}
			push_stdout_msg(" as ");
			push_stdout_msg(nick);
			push_stdout_msg("\n");

			if (debug) {
				char	ibuf[1000];

				snprintf(ibuf, sizeof(ibuf), "%s: rl_line_buffer=0x%p '%s' [%zu] rl_point=%d rl_mark=%d\n",
				    getprogname(), rl_line_buffer, rl_line_buffer, strlen(rl_line_buffer), rl_point, rl_mark);
				push_stdout_msg(ibuf);
			}

			want_info = 0;
		}

		proceed_output(&tasks_net, sock);
		t = time(NULL);
		if (net_timeout &&
		    ts_lastnetinput + net_timeout * (pings_sent + 1) < t) {
			if ((srv_features & Ping) == Ping) {
				push_icb_msg('l', "", 0);
				pings_sent++;
			} else {
				push_icb_msg('n', "", 0);
				ts_lastnetinput = t;
			}
		}
		update_pollfds();
		if (poll(pfd, npfd, poll_timeout) == -1) {
			if (errno == EINTR)
				continue;
			err(1, "poll");
		}

		for (i = 0; i < MainFDCount; i++)
			if ((pfd[i].revents & (POLLERR|POLLHUP|POLLNVAL)))
				errx(1, "error occured on %s", stream_names[i]);

		if (state == Connecting) {
			// the only cause we're here is that connect(2) succeeded
			state = Connected;
			push_stdout_msg("connected\n");
			continue;
		}

		if ((pfd[Stdin].revents & POLLIN))
			rl_callback_read_char();
		if ((pfd[Network].revents & POLLIN)) {
			ts_lastnetinput = time(NULL);
			pings_sent = 0;
			while (!want_exit && (msg = get_next_icb_msg(&msglen)) != NULL)
				proceed_icb_msg(msg, msglen);
		} else if (net_timeout &&
		    ts_lastnetinput + net_timeout * max_pings < t) {
			push_stdout_msg("Server timed out, exiting\n");
			want_exit = 1;
		}
		prepare_stdout();
		proceed_output(&tasks_stdout, STDOUT_FILENO);
		restore_rl();
		proceed_history();
	}
	return 0;
}

int
test_cmd(int count, int key) {
	(void)count;
	(void)key;
	return 0;
}
