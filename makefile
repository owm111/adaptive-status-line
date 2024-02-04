include config.mk

CFLAGS += -Wall -Wextra -Wpedantic -std=c99
CPPFLAGS += -D_XOPEN_SOURCE=700

all: astatus

clean:
	$(RM) astatus

.PHONY: all clean
