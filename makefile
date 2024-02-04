include config.mk

VERSION = 1.0

CFLAGS += -Wall -Wextra -Wpedantic -std=c99
CPPFLAGS += -D_XOPEN_SOURCE=700 -DVERSION=\"$(VERSION)\"

all: astatus

clean:
	$(RM) astatus README.bak

install: astatus
	install -m755 -D -t $(BINDIR) astatus
	install -m644 -D -t $(MANDIR)/man1 astatus.1

uninstall:
	$(RM) $(BINDIR)/astatus $(MANDIR)/man1/astatus.1

.PHONY: all clean install uninstall

README.md: astatus.1
	mv README.md README.bak
	sed '/^Manual Page/,$$d' <README.bak >README.md
	printf 'Manual Page\n-----------\n\n```\n' >>README.md
	nroff -Tutf8 -mdoc astatus.1 | sed 's/\x1b\[[^m]*m//g' \
		| col -b -x >>README.md
	printf '```\n' >>README.md
