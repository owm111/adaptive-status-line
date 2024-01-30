CFLAGS += -g -Og -Wall -Wextra -Wpedantic -std=c99
CPPFLAGS += -D_POSIX_C_SOURCE=200809L

ifneq ($(NOX),1)
CPPFLAGS += -DX
LDLIBS += -lX11
endif

all: astatus

clean:
	$(RM) astatus

.PHONY: all clean
