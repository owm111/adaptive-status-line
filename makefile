CFLAGS += -g -Og -Wall -Wextra -Wpedantic -std=c99
CPPFLAGS += -D_XOPEN_SOURCE=700

ifneq ($(NOX),1)
CPPFLAGS += -DX
LDLIBS += -lX11
endif

ifneq ($(NOALSA),1)
CPPFLAGS += -DALSA
LDLIBS += -lasound
endif

all: astatus

clean:
	$(RM) astatus

.PHONY: all clean
