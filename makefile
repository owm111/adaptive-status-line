CFLAGS += -g -Og -Wall -Wextra -Wpedantic -std=c99
CPPFLAGS += -D_POSIX_C_SOURCE=2

all: astatus

clean:
	$(RM) astatus

.PHONY: all clean
