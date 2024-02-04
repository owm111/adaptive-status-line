include config.mk

CFLAGS += -Wall -Wextra -Wpedantic -std=c99
CPPFLAGS += -D_XOPEN_SOURCE=700

all: astatus

clean:
	$(RM) astatus

install: astatus
	install -m755 -D -t $(BINDIR) astatus

uninstall:
	$(RM) $(BINDIR)/astatus

.PHONY: all clean install uninstall
