#
# This is an OpenBSD-specific Makefile.
# To build oicb under other OSes, please use CMake or write your own Makefile.
#
PROG =		oicb
DPADD +=	${LIBREADLINE} ${LIBCURSES}
LDADD +=	-lreadline -lcurses

BINDIR ?=	/usr/local/bin
MANDIR ?=	/usr/local/man/man

CFLAGS +=	-Wall -DHAVE_PLEDGE -DHAVE_UNVEIL

# debugging helpers
.PHONY: srv tcpdump test test2

test: all
	./${PROG} -d -t 3 tester@localhost:icb hall

test2: all
	./${PROG} -d -d -t 0 tester2@localhost:icb hall

# relies on net/icbd being installed and set up
srv:
	icbd -d -v -v -v -C

tcpdump:
	$${SUDO:-doas} tcpdump -ni lo0 -Xs1500 port icb

.include <bsd.prog.mk>
