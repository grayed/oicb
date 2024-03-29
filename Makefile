#
# This is an OpenBSD-specific Makefile.
# To build oicb under other OSes, please use CMake or write your own Makefile.
#
PROG =		oicb
SRCS =		chat.c history.c oicb.c private.c utf8.c
DPADD +=	${LIBREADLINE} ${LIBCURSES}
LDADD +=	-lreadline -lcurses

BINDIR ?=	/usr/local/bin
MANDIR ?=	/usr/local/man/man

CFLAGS +=	-DHAVE_PLEDGE -DHAVE_UNVEIL
CFLAGS +=	-Wall -Wextra -Wno-unused
CFLAGS +=	-Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations
CFLAGS +=	-Wshadow -Wpointer-arith -Wcast-qual -Wsign-compare

# debugging helpers
.PHONY: srv tcpdump client1 client2

client1: all
	./${PROG} -d -t 3 tester@localhost:icb hall

client2: all
	./${PROG} -d -d -t 0 tester2@localhost:icb hall

# relies on net/icbd being installed and set up
srv:
	icbd -d -v -v -v -C

tcpdump:
	$${SUDO:-doas} tcpdump -ni lo0 -Xs1500 port icb

.include <bsd.prog.mk>
