/*	$OpenBSD: utf8.c,v 1.2 2016/01/18 19:06:37 schwarze Exp $	*/

/*
 * Copyright (c) 2015, 2016 Ingo Schwarze <schwarze@openbsd.org>
 * Copyright (c) 2021 Vadim Zhukov <zhuk@openbsd.org>
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

#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>

#include "utf8.h"

/*
 * Return -1 for string containing invalid bytes or non-printable characters,
 * columns length for valid ones.
 * Empty strings are considered valid and result in 0 being returned.
 */
int
mbsvalidate(const char *mbs)
{
	wchar_t	  wc;
	int	  len;  /* length in bytes of UTF-8 encoded character */
	int	  width;  /* display width of a single Unicode char */
	int	  total_width;  /* display width of the whole string */

	for (total_width = 0; *mbs != '\0'; mbs += len) {
		if ((len = mbtowc(&wc, mbs, MB_CUR_MAX)) == -1)
			return -1;
		else if ((width = wcwidth(wc)) == -1)
			total_width++;
		else
			total_width += width;
	}
	return total_width;
}

/*
 * Finds place where the given string may be split, no longer than maxbytes.
 * If there is whitespace or punctuation, the last one in the given bounds is
 * used as last character.
 * Otherwise, the string is simply split between multi-byte characters.
 * Returned value is always not exceeding maxbytes.
 */
size_t
mbsbreak(const char *mbs, size_t maxbytes) {
	wchar_t		  wc;
	int		  len;  /* length in bytes of UTF-8 encoded character */
	const char	 *lastgood = NULL;  /* position of last space or punctuation */
	const char	 *p;

	for (p = mbs; *p != '\0';) {
		if ((len = mbtowc(&wc, p, MB_CUR_MAX)) == -1)
			len = 1;
		if (p + len > mbs + maxbytes)
			break;
		p += len;
		if (iswblank(wc) || iswpunct(wc))
			lastgood = p;
	}
	if (lastgood != NULL)
		p = lastgood;
	return (size_t)(p - mbs);
}
