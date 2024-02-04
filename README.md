adaptive status line
====================

This is an opinionated, adaptive status line generator for Linux,
inspired by [slstatus](https://tools.suckless.org/slstatus/). It
provides system status information to other programs. By default,
it writes to the standard output to communicate with programs
like [dvtm](https://github.com/martanne/dvtm), but it can also
write to the `WM_NAME` X window property for programs like
[dwm](https://dwm.suckless.org).

Unlike slstatus, astatus can automatically detect what status information
it should provide for the current system without any configuration. This
allows the same program to be used on multiple systems without change
or additional setup (hence adaptive), but it also means the user does
not have much control over what it will output, how the output looks,
etc. (hence opinionated).

The other unique feature of astatus is that, when it detects that a status
is "critical," it will flash a warning message in hopes of catching the
user's attention.

Supported status information:

- Wireless network interface status and signal strengths.
- Storage drive utilization and available space.
- Memory utilization.
- Processor load.
- ALSA volume and mute status.
- Battery status and capacity level.
- Date and time.

Requirements
------------

astatus needs a C99 compiler, libc with SUSv4 support, and make to
build. It optionally requires libX11 and libasound, but these dependencies
can be disabled in config.mk.

At runtime, astatus will need Linux's proc(5) and sysfs(5) interfaces.

Installation
------------

First, edit config.mk to match your local setup and needs.

To install astatus: (may require root permissions)

	make clean install

To uninstall astatus: (also may require root permissions)

	make uninstall

Manual Page
-----------

```
ASTATUS(1)                  General Commands Manual                 ASTATUS(1)

NAME
       astatus — adapative status line

SYNOPSIS
       astatus [-v] [-1] [-s] [-x]

DESCRIPTION
       astatus  is  a  small  tool  for providing system status information to
       other programs.  By default, it writes to the standard output to commu‐
       nicate with programs like dvtm(1), but it can also write to the WM_NAME
       X window property for programs like dwm(1); see “OPTIONS” for details.

       astatus collects status information about the following items.  If  the
       current system cannot provide information about one of the items (e.g.,
       a  system  that does not have any wireless network interfaces), it will
       be silently ignored.

       •   Wireless network interface statuses and signal strengths.

       •   Storage drive utilization and available space.

       •   Memory utilization.

       •   Processor load.

       •   ALSA volume and mute status.

       •   Battery status and capacity level.

       •   Date and time.

       If astatus determines that one of these items is at a  critical  level,
       it  will flash a warning message in hopes of catching the user's atten‐
       tion.

OPTIONS
       -v      Print the version to the standard error, then exit.

       -1      Write once and exit.

       -s      Write to the standard output (the default behavior).

       -x      Write to WM_NAME instead of the standard output.

CUSTOMIZATION
       astatus can be customized by modifying  and  (re)compiling  the  source
       code.  This keeps it fast, secure, and simple.

SIGNALS
       astatus responds to the following signals:

       USR1  Causes astatus to retrieve and print new status information imme‐
             diately.
       INT   Exits.

FILES
       astatus  retrieves  much of the status information from Linux's proc(5)
       and sysfs(5) interfaces.

EXIT STATUS
       The astatus utility exits 0 on success, and >0 if an error occurs.

EXAMPLES
       The following command line shows astatus being used as  a  status  line
       for dvtm(1) via process redirection.
             % dvtm -s <(astatus)

       The  following  script does the same as the previous command, but using
       named pipes instead of process direction (which is not available in all
       shells).  This example is based off of the dvtm-status utility.

             #!/bin/sh
             fifo=/tmp/dvtm-status.$$
             mkfifo -m600 $fifo
             astatus >$fifo &
             status_pid=$!
             dvtm -s $fifo "$@"
             kill $status_pid
             wait $status_pid
             rm $fifo

       To use astatus with dwm(1), add the following line to .xinitrc (or  any
       other X startup file):
             % astatus -x &

       The  following  command  line shows pkill(1) being used to have astatus
       print new status information  instantly  after  adjusting  the  volume.
       Commands  like  this  can be bound to keys in dwm(1) and similar window
       managers to update the status line when the user  adjusts  the  volume,
       etc.
             % amixer sset Master 1%-; pkill -USR1 astatus

SEE ALSO
       dvtm(1), dwm(1), pkill(1), slstatus(1)

Nixpkgs                           2024-02-04                        ASTATUS(1)
```
