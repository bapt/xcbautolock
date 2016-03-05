PROG=	xcbautolock
MAN=	xcbautolock.1
SRCS=	xcbautolock.c
OBJS=	$(SRCS:.c=.o)
CFLAGS+=	-Wall `pkg-config --cflags xcb xcb-screensaver`
LDFLAGS+=	`pkg-config --libs xcb xcb-screensaver`
PREFIX?=	/usr/local
BINDIR?=	$(PREFIX)/bin
MANDIR?=	$(PREFIX)/man/man1
INSTALL_PROG?=	install -s -m 755
INSTALL_MAN?=	install -m 644

.c.o:
	$(CC) -c $(CPPFLAGS) $(CFLAGS) -o $@ $<

all: $(PROG) $(MAN).gz

$(PROG): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

$(MAN).gz: $(MAN)
	gzip -kn $(MAN)

clean:
	rm -f $(PROG) *.o *.gz

install: $(PROG) $(MAN).gz
	$(INSTALL_PROG) $(PROG) $(DESTDIR)$(BINDIR)
	$(INSTALL_MAN) $(MAN).gz $(DESTDIR)$(MANDIR)

