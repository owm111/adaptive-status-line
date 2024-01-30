adaptive status line
====================

This is an opinionated, adaptive status line generator for Linux that
was inspired [slstatus](https://tools.suckless.org/slstatus/).

Requirements
------------

Compile time dependencies:

- (GNU?) make
- C compiler & libc with 1992 POSIX support

Run time dependencies:

- (GNU?) awk
- df (for disk support to function)
- numfmt (for disk support to function)
- Linux procfs
- Linux sysfs
- amixer (for ALSA support to function)

In theory, the program will run if none of these are present, but it
will not do very much.
