# paths
PREFIX ?= /usr/share
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/man

# flags (the "necessary" ones are in the makefile)
CFLAGS ?= -g -Os
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?= -lc

# x11 support: to disable, call make with NOX=1 or comment out this block
ifneq ($(NOX),1)
CPPFLAGS += -DX
LDLIBS += -lX11
endif

# alsa support: to disable, call make with NOALSA=1 or comment out this block
ifneq ($(NOALSA),1)
CPPFLAGS += -DALSA
LDLIBS += -lasound
endif
