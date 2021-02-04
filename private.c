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
#include <err.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <readline/readline.h>

#include "oicb.h"
#include "private.h"


static int	match_nick_from_history(const char *prefix, size_t prefixlen,
	                        int forward);

char		 priv_chats_nicks[PRIV_CHATS_MAX][NICKNAME_MAX];
int		 priv_chats_cnt;
int		 repeat_priv_nick;
int		 prefer_long_priv_cmd;


int
match_nick_from_history(const char *prefix, size_t prefixlen, int forward) {
	int	i;

	if (forward) {
		for (i = 0; i < priv_chats_cnt; i++)
			if (!strncmp(prefix, priv_chats_nicks[i], prefixlen))
				return i;
	} else {
		for (i = priv_chats_cnt - 1; i >= 0; i--)
			if (!strncmp(prefix, priv_chats_nicks[i], prefixlen))
				return i;
	}
	return forward ? 0 :  priv_chats_cnt - 1;
}

//
// The logic as follows:     [a] forward == 1; [b] forward == 0
//
//  1.  If private chat nicknames history is empty, just beep.
//
//  2a. If this is a public chat message, switch to last used private chat.
//  2b. If this is a public chat message, switch to last used private chat.
//
//  3.  If this is not either public or private chat line, just beep.
//
//  4a. If this is a private chat command without nickname, switch to last
//      used private chat.
//  4b. If this is a private chat command without nickname, switch to least
//      used private chat.
//
//  5a. If the private chat nickname is the oldest used in nick history,
//      clear private command, making the message public.
//  5b. If the private chat nickname is the newest used in nick history,
//      clear private command, making the message public.
//
//  6.  If private chat nickname is not remembered, but there is remembered
//      nickname which starts like this, fill the latter nickname fully
//      (matching first/last nickname from history depending on forward == 1/0)
//
//  7a. If there is no matching private chat nickname in history, switch to the
//      last used private chat.
//  7b. If there is no matching private chat nickname in history, switch to the
//      least used private chat.
//
//  8a. Now this is a non-oldest remembered private chat nickname: replace
//      nickname in private commmand with older one.
//  8b. Now this is a non-oldest remembered private chat nickname: replace
//      nickname in private commmand with newer one.
//
int
cycle_priv_chats(int forward) {
	struct line_cmd cmd;
	int		i;
	int		newidx;		// index from history to use
	int		oldcurpos;	// initial rl_point value

	enum {
		BEFORE_NICK,
		INSIDE_NICK,
		AFTER_NICK
	} cursor_zone = AFTER_NICK;

	if (debug >= 3)
		warnx("%s: forward=%d\n", __func__, forward);

	if (priv_chats_cnt == 0) {
		// (1)
		putchar('\007');
		return 0;
	}

	oldcurpos = rl_point;

	if (parse_cmd_line(rl_line_buffer, &cmd)) {
		// (3), (4), (5), (6), (7) or (8)
		if (debug >= 3)
			warnx("%s: 3-4-5-6-7-8", __func__);

		if (!cmd.is_private) {
			// (3)
			putchar('\007');
			return 0;
		}
		prefer_long_priv_cmd = cmd.cmd_name_len == 3;

		if (cmd.peer_nick_offset) {
			if (rl_point <= cmd.peer_nick_offset)
				cursor_zone = BEFORE_NICK;
			else if (rl_point < cmd.peer_nick_offset + cmd.peer_nick_len)
				cursor_zone = INSIDE_NICK;
		}

		if (cmd.peer_nick) {
			// (5), (6), (7) or (8)

			for (i = 0; i < priv_chats_cnt; i++)
				if (strlen(priv_chats_nicks[i]) == (size_t)cmd.peer_nick_len &&
				   memcmp(cmd.peer_nick, priv_chats_nicks[i], (size_t)cmd.peer_nick_len) == 0)
					break;
			if (( forward && i == priv_chats_cnt - 1) ||
			    (!forward && i == 0)) {
				// (5)
				rl_delete_text(0, cmd.private_prefix_len);
				rl_point -= cmd.private_prefix_len;
				if (rl_point < 0)
					rl_point = 0;
				return 0;
			}

			if (i == priv_chats_cnt) {
				// (6) or (7): do prefix matching check
				newidx = match_nick_from_history(cmd.peer_nick,
				    (size_t)cmd.peer_nick_len, forward);
				goto replace_nick;
			}

			// (8)
			newidx = i + (forward ? 1 : -1);
			goto replace_nick;
		}

		// (4)
		newidx = forward ? 0 : priv_chats_cnt - 1;
	} else {
		// (2)
		rl_point = 0;
		if (prefer_long_priv_cmd) {
			rl_insert_text("/msg ");
			cmd.peer_nick_offset = 5;
			cmd.private_prefix_len = 5;
		} else {
			rl_insert_text("/m ");
			cmd.peer_nick_offset = 3;
			cmd.private_prefix_len = 3;
		}
		oldcurpos += cmd.peer_nick_offset + 1;
		newidx = forward ? 0 : priv_chats_cnt - 1;
	}

	// (2), (4), (6), (7) or (8)

replace_nick:
	// cmd fields used:
	//   peer_nick_offset
	//   peer_nick_len
	//   private_prefix_len

	if (debug >= 2) {
		warnx("%s: replace_nick: rl_line_buffer='%s' rl_point=%d oldcurpos=%d newidx=%d",
		    __func__, rl_line_buffer, rl_point, oldcurpos, newidx);
		warnx("%s: replace_nick: peer_nick_offset=%d peer_nick_len=%d cursor_zone=%d",
		    __func__, cmd.peer_nick_offset, cmd.peer_nick_len, cursor_zone);
	}

	if (cmd.peer_nick_len)
		rl_delete_text(cmd.peer_nick_offset, cmd.private_prefix_len);
	rl_point = cmd.peer_nick_offset;
	rl_insert_text(priv_chats_nicks[newidx]);
	rl_insert_text(" ");

	// restoring cursor position
	switch (cursor_zone) {
	case BEFORE_NICK:
		rl_point = oldcurpos;
		break;

	case INSIDE_NICK:
		// play safe
		rl_point = cmd.peer_nick_offset + cmd.peer_nick_len + 1;
		break;

	case AFTER_NICK:
		rl_point = oldcurpos
		    + (int)strlen(priv_chats_nicks[newidx])
		    - cmd.peer_nick_len;
		break;
	}
	return 0;
}

int
list_priv_chats_nicks() {
	int	i;

	if (priv_chats_cnt == 0) {
		push_stdout("there are no private chats yet\n");
		return 0;
	}

	push_stdout("there are %d private chats:\n", priv_chats_cnt);
	for (i = 0; i < priv_chats_cnt; i++)
		push_stdout("  * %s\n", priv_chats_nicks[i]);
	return 0;
}

// Input: private message nick and text, or NULLs for public message.
void
update_nick_history(const char *peer_nick, const char *msg) {
	if (debug >= 2)
		warnx("%s: peer_nick='%s' msg='%s'", __func__, peer_nick, msg);

	if (peer_nick != NULL) {
		size_t	nicklen;
		int	i;

		nicklen = strlen(peer_nick);
		if (nicklen >= NICKNAME_MAX) {
			push_stdout("%s: warning: nickname is too long\n",
			                getprogname());
			return;
		}

		if (priv_chats_cnt > 0 &&
		    strcmp(priv_chats_nicks[0], peer_nick) == 0)
			return;		// already topmost one
		for (i = 1; i < priv_chats_cnt; i++) {
			if (strcmp(priv_chats_nicks[i], peer_nick) == 0) {
				if (debug >=2)
					warnx("%s: found %s", __func__, priv_chats_nicks[i]);
				// make current nick the newest one
				memmove(priv_chats_nicks[1], priv_chats_nicks[0], i * NICKNAME_MAX);
				goto set_top_nick;
			}
		}

		// add nick to history, possibly kicking out oldest one
		memmove(priv_chats_nicks[1], priv_chats_nicks[0],
		    (PRIV_CHATS_MAX - 1) * NICKNAME_MAX);
		if (priv_chats_cnt < PRIV_CHATS_MAX - 1)
			priv_chats_cnt++;

set_top_nick:
		if (debug >= 2)
			warnx("%s: copying '%s' [%zu] to %p",
			    __func__, peer_nick, nicklen, priv_chats_nicks[0]);
		memcpy(priv_chats_nicks[0], peer_nick, nicklen);
		memset(priv_chats_nicks[0] + nicklen, 0, NICKNAME_MAX - nicklen);
	}
}
