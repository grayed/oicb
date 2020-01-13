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
#include <ctype.h>
#include <limits.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vis.h>

#include "oicb.h"
#include "chat.h"
#include "history.h"
#include "private.h"


static void	 err_unexpected_msg(char type);
static void	 err_invalid_msg(char type, const char *desc);
static void	 push_icb_msg_ws(char type, const char *src, size_t len);
static void	 push_icb_msg_extended(char type, const char *src, size_t len);

static void	 proceed_chat_msg(char type, const char *author, const char *text);
static void	 proceed_cmd_result(char *msg, size_t len);
static void	 proceed_cmd_result_end(char *msg, size_t len);
static void	 proceed_user_list(char *msg, size_t len);
static void	 proceed_group_list(char *msg, size_t len);

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


/*
 * Queue ICB messages to be sent to server.
 */
void
push_icb_msg(char type, const char *src, size_t len) {
	if (debug >= 2) {
		warnx("%s: asked type '%c' with size %zu: %s, queue %p",
		    __func__, type, len, src, &tasks_net);
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
			err(1, __func__);
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
	if (debug >= 3)
		warnx("%s: there will be %zu messages", __func__, msgcnt);

	// +1 for sizeof(struct icb_task)
	if ((it = calloc(msgcnt + 1, 256)) == NULL)
		err(1, __func__);
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
 * Handle text line coming from libreadline.
 */
void
proceed_user_input(char *line) {
	struct line_cmd	 cmd;
	const char	*p;

	if (line == NULL) {
		want_exit = 1;
		return;
	}

	for (p = line; isspace(*p); p++)
		;
	if (!*p)
		return;    // add some insult like icb(1) does?

	if (parse_cmd_line(line, &cmd)) {
		if (cmd.has_args)
			*cmd.cmd_name_end = '\001';    // separate args

		if (cmd.is_private) {
			char	ch;

			if (!cmd.private_msg)
				return;    // skip empty line
			ch = *cmd.peer_nick_end;
			*cmd.peer_nick_end = '\0';
			update_nick_history(cmd.peer_nick, cmd.private_msg);
			save_history('c', cmd.peer_nick, cmd.private_msg, 0);
			*cmd.peer_nick_end = ch;
			repeat_priv_nick = 1;
			prefer_long_priv_cmd = cmd.cmd_name_len == 3;
		}
		push_icb_msg('h', line + 1, strlen(line) - 1);
		return;
	}

	// public message
	update_nick_history(NULL, NULL);
	save_history('b', nick, line, 0);
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
	size_t		 textlen;
	char		 timebuf[sizeof("[00:00:00]")];
	char		 author_vis[NICKNAME_MAX * 4 + 1];
	char		*text_vis;
	const char	*preuser, *postuser;
	time_t		 t;

	save_history(type, author, text, 1);

	switch (type) {
	case 'c':
		preuser  = postuser = "*";
		break;
	case 'd':
		preuser  = "[=";
		postuser = "=]";
		break;
	case 'e':
	case 'k':
		preuser  = postuser = "!";
		break;
	case 'f':
		preuser  = "{";
		postuser = "}";
		break;
	default:
		preuser  = "<";
		postuser = ">";
	}

	t = time(NULL);
	strftime(timebuf, sizeof(timebuf), "[%H:%M:%S]", localtime(&t));

	strnvis(author_vis, author, sizeof(author_vis), VIS_SAFE|VIS_NOSLASH);

	// libbsd lacks stravis()
	textlen = strlen(text);
	if ((text_vis = malloc(textlen + 1)) == NULL)
		err(1, __func__);
	strvis(text_vis, text, VIS_SAFE|VIS_NOSLASH);

	push_stdout("%s %s%s%s %s\n",
	                timebuf, preuser, author_vis, postuser, text_vis);
	free(text_vis);
}

void
proceed_cmd_result(char *msg, size_t len) {
	(void)len;
	push_stdout_untrusted(msg);
	push_stdout("\n");
}

void
proceed_cmd_result_end(char *msg, size_t len) {
	(void)len;
	push_stdout_untrusted(msg);
	push_stdout("\n");
	state = Chat;
}

void
proceed_user_list(char *msg, size_t len) {
	char		*p, *endptr;
	const char	*peer_nick, *ident, *srcaddr;
	long long	 signedon, idle;
	struct tm	 tm;

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
		push_stdout("*");
	else
		push_stdout(" ");
	peer_nick = p + 1;
	p = strchr(peer_nick, '\001');
	if (p != NULL)
		*p = '\0';
	push_stdout_untrusted(peer_nick);

	if (p == NULL)
		goto end;
	p++;
	idle = strtoll(p, &endptr, 10);
	*endptr = '\0';
	push_stdout(" % 7llds", idle);
	p = strchr(endptr + 1, '\001');
	if (p == NULL)
		goto end;
	/* this field is always zero, no interest */
	p++;
	signedon = strtoll(p, &endptr, 10);
	if (*endptr != '\001' && *endptr != '\0')
		goto end;
	localtime_r((time_t*)&signedon, &tm);
	// TODO: omit date when not needed? print 'today'/'yesterday'?..
	push_stdout(" %d-%02d-%02d %02d:%02d:%02d",
	            tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
	            tm.tm_hour, tm.tm_min, tm.tm_sec);
	if (*endptr == '\0')
		goto end;

	ident = endptr + 1;
	p = strchr(ident, '\001');
	if (p != NULL)
		*p = '\0';
	push_stdout("\t");
	push_stdout_untrusted(ident);
	if (p == NULL)
		goto end;

	srcaddr = p + 1;
	p = strchr(srcaddr, '\001');
	if (p != NULL)
		*p = '\0';
	push_stdout("\t");
	push_stdout_untrusted(srcaddr);

end:
	push_stdout("\n");
}

void
proceed_group_list(char *msg, size_t len) {
	int		 name_out_len;
	char		*name, *topic, *msgid;
	const int	 min_name_len = 30;

	(void)len;

	name = msg;
	if ((topic = strchr(name, '\001')) == NULL) {
		warnx("invalid group info line received, ignoring");
		return;
	}
	*topic++ = '\0';
	if ((msgid = strchr(topic, '\001')) != NULL)
		*msgid++ = '\0';

	push_stdout(strcmp(name, room) ? " " : "*");
	name_out_len = push_stdout_untrusted(name);
	if (name_out_len < min_name_len)
		push_stdout("%*s", min_name_len - name_out_len, "");

	if (*topic) {
		push_stdout(" <");
		push_stdout_untrusted(topic);
		push_stdout(">");
	}
	if (msgid) {
		push_stdout(" [");
		push_stdout_untrusted(msgid);
		push_stdout("]");
	}
	push_stdout("\n");
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
		push_stdout("Logged in to room %s as %s\n", room, nick);
		state = Chat;
		break;

	case 'b':	// open message
	case 'c':	// private message
	case 'd':	// status message
	case 'f':	// important message
	{
		char	*text;
		if (state != Chat)
			err_unexpected_msg(type);
		if ((text = strchr(msg, '\001')) == NULL)
			err_invalid_msg(type, "missing text");
		*text++ = '\0';
		proceed_chat_msg(type, msg, text);
		break;
	}

	case 'e':	// error
		if (state != Chat)
			want_exit = 1;
		if (strcmp(msg, "Undefined message type 108") == 0) {
			/* server doesn't support ping-pong */
			srv_features &= (~Ping);
			/* XXX set socket timeout options? */
			if (debug)
				warnx("server doesn't support ping-pong,"
				    " switching to no-op messages");
			break;
		}
		push_stdout("\007");
		proceed_chat_msg(type, hostname, msg);
		break;

	case 'g':       // exit
		if (state != Chat)
			err_unexpected_msg(type);
		push_stdout("ICB: server said bye-bye\n");
		want_exit = 1;
		break;

	case 'i':       // command result
	{
		int	 i;
		char	*outtype;

		if (state != Chat)
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
			err(1, __func__);
		push_icb_msg('a', p, strlen(p));
		free(p);
		state = LoginSent;
		break;
	}

	case 'k':       // beep
		if (state != Chat)
			err_unexpected_msg(type);
		proceed_chat_msg(type, "SERVER", "\007BEEP!");
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
		push_stdout("unsupported message of type '%c', ignored\n",
		                type);
	}
}
