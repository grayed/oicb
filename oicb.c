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
#include <locale.h>
#include <netdb.h>
#include <poll.h>
#include <resolv.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <vis.h>

#include <signal.h>

#include <readline/readline.h>

#include "oicb.h"
#include "chat.h"
#include "history.h"
#include "private.h"
#include "utf8.h"

#ifndef HAVE_RL_BIND_KEYSEQ
static inline int	rl_bind_keyseq(const char *keyseq, int(*function)(int, int));
#endif

enum ICBState state = Connecting;
enum SrvFeatures srv_features = Ping;

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

struct icb_task_queue tasks_stdout, tasks_net;

int		 debug = 0;
int		 sock = -1, histfile = -1;
volatile int	 want_exit = 0;
volatile int	 want_info = 0;
char		*nick, *hostname, *room;
char		*o_rl_buf = NULL;
int		 o_rl_point, o_rl_mark;
int		 pings_sent = 0;
int		 utf8_ready = 0;


void	 usage(const char *msg);
void	 pledge_me(void);
int	 test_cmd(int count, int key);

size_t	 push_data(int fd, char *data, size_t len);
void	 proceed_output(struct icb_task_queue *q, int fd);
char	*get_next_icb_msg(size_t *msglen);

void	 update_pollfds(void);
#ifdef SIGINFO
void	 siginfo_handler(int sig);
#endif
int	 siginfo_cmd(int count, int key);
void	 prepare_stdout(void);
void	 restore_rl(void);
void	 icb_connect(const char *addr, const char *port);

char	*null_completer(const char *text, int cmpl_state);

// readline wrappers
int	cycle_priv_chats_forward(int count, int key);
int	cycle_priv_chats_backward(int count, int key);
int	list_priv_chats_nicks_wrapper(int count, int key);



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
		errx(1, "%s: already called (internal error)", __func__);
	o_rl_buf = strdup(rl_line_buffer);
	if (o_rl_buf == NULL)
		err(1, __func__);
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
 * Queue text coming from trusted source to be displayed.
 *
 * The data at 'text' won't be accessed after return, its contents will be
 * copied to internal buffer for the further processing.
 *
 * Returns number of bytes queued, not including terminating NUL.
 */
int
push_stdout(const char *text, ...) {
	struct icb_task	*it;
	int		 len;
	va_list		 ap;

	va_start(ap, text);
	len = vsnprintf(NULL, 0, text, ap);
	va_end(ap);
	if (len == 0)
		return 0;
	it = calloc(1, sizeof(struct icb_task) + len + 1);
	if (it == NULL)
		err(1, __func__);
	it->it_len = len + 1;
	va_start(ap, text);
	vsnprintf(it->it_data, len + 1, text, ap);
	va_end(ap);
	SIMPLEQ_INSERT_TAIL(&tasks_stdout, it, it_entry);
	return len;
}

/*
 * Queue text coming from possibly untrusted source to be displayed.
 * The data will be processed with strvis(3) internally, if needed.
 *
 * The data at 'text' won't be accessed after return, its contents will be
 * copied to internal buffer for the further processing.
 *
 * Returns number of bytes queued, not including terminating NUL.
 */
int
push_stdout_untrusted(const char *text, ...) {
	struct icb_task	*it, *tmp = NULL;
	size_t		 len, buflen;
	va_list		 ap;
	char		*fmtbuf;

	va_start(ap, text);
	len = vsnprintf(NULL, 0, text, ap);
	va_end(ap);
	if (len == 0)
		return 0;

	it = calloc(1, sizeof(struct icb_task) + len + 1);
	if (it == NULL)
		err(1, __func__);

	va_start(ap, text);
	vsnprintf(it->it_data, len + 1, text, ap);
	va_end(ap);

	if (utf8_ready && mbsvalidate(it->it_data) != -1) {
		it->it_len = len + 1;
		goto finish;
	}

	buflen = len * 4 + 1;
	tmp = calloc(1, sizeof(struct icb_task) + buflen);
	if (tmp == NULL)
		err(1, __func__);
	tmp->it_len = strvis(tmp->it_data, it->it_data, VIS_SAFE|VIS_NOSLASH|VIS_NL) + 1;
	free(it);
	it = tmp;

finish:
	SIMPLEQ_INSERT_TAIL(&tasks_stdout, it, it_entry);
	return (int)it->it_len;
}

// Input: line input from user
// Returns: 1 if valid command found, 0 otherwise
// Note: 'line' is not modified because struct fields being assigned are not.
int
parse_cmd_line(char *line, struct line_cmd *cmd) {
	char	*p = (char *)line;

	memset(cmd, 0, sizeof(struct line_cmd));
	if (*p != '/') {
		if (debug >= 2)
			warnx("%s: not a command, line='%s'",
			   __func__, line);
		return 0;
	}
	cmd->start = p++;

	if (!*p || isspace(*p)) {
		if (debug >= 2)
			warnx("%s: not a command name, line='%s'",
			   __func__, line);
		return 0;
	}
	cmd->cmd_name = p;

	while (*p && !isspace(*p))
		p++;
	cmd->cmd_name_end = p;
	cmd->cmd_name_len = (int)(p - cmd->cmd_name);

	if (*p)
		cmd->has_args = 1;
	else
		goto end;

	if ((cmd->cmd_name_len == 1 && cmd->cmd_name[0] == 'm') ||
	    (cmd->cmd_name_len == 3 && memcmp(cmd->cmd_name, "msg", 3) == 0)) {
		cmd->is_private = 1;

		while (isspace(*p))
			p++;
		if (!*p) {
			cmd->private_prefix_len = (int)(p - line);
			goto end;
		}
		cmd->peer_nick = p;
		cmd->peer_nick_offset = (int)(p - line);

		while (*p && !isspace(*p))
			p++;
		cmd->peer_nick_end = p;
		cmd->peer_nick_len = (int)(p - cmd->peer_nick);
		cmd->private_prefix_len = (int)(p - line);

		if (isspace(*p)) {
			cmd->private_msg = ++p;
			cmd->private_prefix_len++;
		}
	}

end:
	if (debug >= 2)
		warnx("%s: cmd_name='%.*s' nick_name='%.*s' private_msg='%s'",
		    __func__, cmd->cmd_name_len, cmd->cmd_name,
		    cmd->peer_nick_len, cmd->peer_nick,
		    cmd->private_msg ? cmd->private_msg : "(null)");
	return 1;
}

int
cycle_priv_chats_forward(int count, int key) {
	(void)key;
	while (count-- > 0)
		cycle_priv_chats(1);
	return 0;
}

int
cycle_priv_chats_backward(int count, int key) {
	(void)key;
	while (count-- > 0)
		cycle_priv_chats(0);
	return 0;
}

int
list_priv_chats_nicks_wrapper(int count, int key) {
	(void)count;
	(void)key;
	list_priv_chats_nicks();
	return 0;
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
	err(2, __func__);
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
			err(1, "%s: malloc", __func__);

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
				err(1, "%s: reallocarray", __func__);
			buf = nbuf;
			bufsize *= 2;
		}
		nread = read(sock, buf + bufread, bufsize - bufread - 1);
		if (nread < 0) {
			if (errno != EAGAIN)
				err(1, "%s: read", __func__);
			break;
		} else if (nread == 0) {
			push_stdout("Server %s closed connection, exiting...\n",
			                hostname);
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
			err(1, __func__);
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
		errx(1, "could not resolve host/port name: %s",
		    gai_strerror(ec));
	for (p = res; p != NULL; p = p->ai_next) {
		if ((sock = socket(p->ai_family, p->ai_socktype|SOCK_NONBLOCK,
		    p->ai_protocol)) == -1) {
			warn("%s: socket", __func__);
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
			push_stdout("Connecting to %s ... ", hostname);
		} else {
			state = Connected;
			push_stdout("Connected to %s\n", hostname);
		}
		freeaddrinfo(res);
		return;
	}
	freeaddrinfo(res);
	err(1, "could not connect");
}

void
pledge_me() {
#ifdef HAVE_PLEDGE
	int	result;
#endif

#ifdef HAVE_UNVEIL
	if (enable_history) {
		if (unveil(history_path, "wc") == -1)
			err(1, "history unveil");
	}
	if (unveil(NULL, NULL) == -1)
		err(1, "final unveil");
#endif

#ifdef HAVE_PLEDGE
	if (enable_history)
		result = pledge("stdio wpath cpath tty", NULL);
	else
		result = pledge("stdio tty", NULL);
	if (result == -1)
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
	const char	*errstr, *locale;

	SIMPLEQ_INIT(&tasks_stdout);
	SIMPLEQ_INIT(&tasks_net);

	locale = setlocale(LC_CTYPE, "");
	if (strstr(locale, ".UTF-8")) {
		utf8_ready = 1;
		if (debug >= 1)
			warnx("UTF-8 support detected");
	}

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
				errx(1, "invalid network timeout: %s",
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
	rl_bind_key(CTRL('p'), list_priv_chats_nicks_wrapper);
	rl_bind_key(CTRL('t'), siginfo_cmd);
	if (debug)
		rl_bind_key(CTRL('x'), test_cmd);

#ifdef SIGINFO
	if (sigaction(SIGINFO, NULL, &sa) == -1)
		warn("sigaction(SIGINFO, NULL)");
	else {
		sa.sa_handler = siginfo_handler;
		if (sigaction(SIGINFO, &sa, NULL) == -1)
			warn("sigaction(SIGINFO, &sa)");
	}
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
			push_stdout("%s: sitting in room %s at %s",
			                getprogname(), room, hostname);
			if (port)
				push_stdout(":%s", port);
			push_stdout(" as %s\n", nick);

			if (debug)
				push_stdout("%s: rl_line_buffer=0x%p '%s' [%zu] rl_point=%d rl_mark=%d\n",
				                getprogname(),
				                rl_line_buffer, rl_line_buffer,
				                strlen(rl_line_buffer),
				                rl_point, rl_mark);

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
			push_stdout("connected\n");
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
			push_stdout("Server timed out, exiting\n");
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
