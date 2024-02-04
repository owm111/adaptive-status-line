/*
adaptive status line

The basic idea for the program is as follows:

- Define functions that collect status information and print them to a
stream (FILE *).
- Put those functions into an array so they can be iterated over.
- Pass either stdout (for -s) or buffer in memory (via fmemopen(3))
that will be used to set WM_NAME (for -x) to each function.
- Call the functions in a loop, wait a few seconds or until USR1 is
received between each iteration, repeating until TERM or INT is received.
*/

/*
Unix headers.
*/

#include <glob.h>
#include <limits.h>
#include <mntent.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>

/*
For optional libraries, I chose to use preprocessor macros to recreate
the relevants parts of the interfaces such that they only return error
values. I chose this method since it reduces the amount of #ifdef/#endifs
throughout the code, which should increase clarity. The compiler should
catch when the constant values are used as conditions optimize them away,
so the performance of this method compared to #ifdef/#endifs throughout
the code should be similar. The downside of this method is that it's a
lot of noise towards the top of the source, and it will require editing
if different functions are needed.
*/

#ifdef X
#include <X11/Xlib.h>
#else /* X */
#define DefaultRootWindow(x) (-1)
#define Display void
#define Window int
#define XCloseDisplay(x) ((void)(x), -1)
#define XFlush(dpy) NULL
#define XOpenDisplay(x) ((void)(x), NULL)
#define XStoreName(x, y, z) ((void)(x), (void)(y), (void)(z), -1)
#endif /* X */

#ifdef ALSA
#include <alloca.h>
#include <alsa/asoundlib.h>
#else /* ALSA */
#define SND_MIXER_SCHN_MONO 0
#define snd_mixer_attach(x, y) (-1)
#define snd_mixer_close(x) (void)(x)
#define snd_mixer_detach(x, y) (void)(y)
#define snd_mixer_elem_t void
#define snd_mixer_find_selem(x, y) NULL
#define snd_mixer_free(x) (void)(x)
#define snd_mixer_load(x) (-1)
#define snd_mixer_open(x, y) (-1)
#define snd_mixer_selem_get_playback_switch(x, y, z) (-1)
#define snd_mixer_selem_get_playback_volume(x, y, z) (-1)
#define snd_mixer_selem_get_playback_volume_range(x, y, z) (-1)
#define snd_mixer_selem_id_alloca(x) (void)(x)
#define snd_mixer_selem_id_set_index(x, y)
#define snd_mixer_selem_id_set_name(x, y)
#define snd_mixer_selem_id_t void
#define snd_mixer_selem_register(x, y, z) (-1)
#define snd_mixer_t void
#endif /* ALSA */

/*
General preprocessor macros.
*/

/* Return the amount of elements in an array */
#define LEN(arr) (sizeof(arr) / sizeof((arr)[0]))
/* Convert constants to string literals. */
#define STRINGIFY(X) #X
/* Convert macros to string literals. */
#define TOSTRING(X) STRINGIFY(X)
/* Convert time milliseconds to a struct timespec pointer. */
#define TIMESPEC(ms) (&(struct timespec){.tv_sec = (ms) / 1000, \
		.tv_nsec = (ms) % 1000 * 1e6})

/*
Another effort to reduce #ifdef/#endifs throughout the source, this time
for argument processing and usage information. XFLAG can be used in the
usage text, and it will give the proper string literal depending on if X
support is enabled or not. XALLOWED can be used as a condition to check
the if an argument contains the -x flag (if X is not supported, XALLOWEDD
will be a constant, false condition so the option will never match).
*/

#ifdef X
#define XFLAG " [-x]"
#define XALLOWED 1
#else /* X */
#define XFLAG ""
#define XALLOWED 0
#endif /* X */

/*
Some constants and string literals.
*/

/* Typing this gets repetitive. */
#define BATTERY_PREFIX "/sys/class/power_supply/"
/* Max length of an net interface name. XXX What should this actually be? */
#define MAX_INTERFACE_LEN 512
/* Limit to the number of disks to display. THis seems reasonable. */
#define MAX_NUM_DISKS 5

/*
Macros that control configuration.
*/

/* ALSA device to watch. */
#define ALSA_DEVICE "default"
/* ALSA mixer to watch within the device. */
#define ALSA_MIXER "Master"
/* String printed between blocks. */
#define SEPARATOR " ┆ "
/* Urgent messages are prefixed with this... */
#define URGENT_PREFIX "!!!! Urgent message:"
/* ...and suffixed with this. */
#define URGENT_SUFFIX "!!!!"
/* Timing and flashing: */
#define INTERVAL 5000 /* ms between refreshes */
#define HOLD_TIME 1500 /* ms to display status when there is an urgent msg */
#define URGENT_FLASH_ON 100 /* ms to flash urgent message "on" for */
#define URGENT_FLASH_OFF 50 /* ms to flash urgent message "off" for */
#define URGENT_FLASHES 20 /* how many times to flash the urgent message */

/*
Global variables declarations.
*/

/* Program name/path used for error messages. */
static char *argv0 = "astatus";
/* Exit the main loop when this becomes true. */
static int done;
/* Print to WM_NAME instead of stdout when this is true. */
static int x = 0;
/* X display to use when x != 0. */
static Display *dpy;
/* A buffer to print the status to when x != 0. */
static char xbuf[4096];
/* Urgent messages are copied here. */
static char urgentmsg[2048];

/*
Some general purpose utilities.
*/

/* Print to stderr & exit. From
https://git.suckless.org/dmenu/file/util.c.html#l10. */
static void
die(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "%s: ", argv0);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	if (fmt[0] && fmt[strlen(fmt) - 1] == ':') {
		fputc(' ', stderr);
		perror(NULL);
	} else {
		fputc('\n', stderr);
	}
	exit(1);
}

/*
Signal handling. The signal handling code was taken from
https://git.suckless.org/slstatus/file/slstatus.c.html#l74. Slstatus
passes SA_RESTART to sigaction for USR1, and I'm not sure why (it seems
to work fine without it).
*/

/* All signals (that we care about) exit, besides USR1. */
static void
onsignal(int signum)
{
	if (signum != SIGUSR1)
		done = 1;
}

/*
Exiting variants of various functions (which die() if an error would be
returned). Not having inline error checking cleans up code, and exiting
on errors from these functions is common here.
*/

static FILE *
efmemopen(void *buf, size_t size, const char *mode)
{
	FILE *result;

	result = fmemopen(buf, size, mode);
	if (result == NULL)
		die("fmemopen:");
	return result;
}

static Display *
eXOpenDisplay(char *display_name)
{
	Display *result;

	result = XOpenDisplay(display_name);
	if (result == NULL)
		die("XOpenDisplay: Failed to open display");
	return result;
}

static void
eXStoreName(Display *dpy, Window w, char *window_name)
{
	int rc;

	rc = XStoreName(dpy, w, window_name);
	if (rc < 0)
		die("XStoreName: Allocation failed");
}

static void
eXCloseDisplay(Display *dpy)
{
	int rc;

	rc = XCloseDisplay(dpy);
	if (rc < 0)
		die("XCloseDisplay: Failed to close display");
}

/*
Wireless network interfaces.

XXX is there a simpler way to do wifi? this is kinda a mess.
The goal is to display the signal strength for any connected interfaces
(can there be more than one?) and any disconnected interfaces. As far
as I can tell based on a hour or so of searching, it does not look
like there is a singular, simple interface that provides all of this
information. The solution I came up with is:

- Use glob on /sys/class/ieee80211 to get a list of wireless interfaces.
- Use /proc/net/wireless to get the signal strength of connected interfaces.
- Use /sys/class/net to get the states of remaining interfaces.

Here are the (potential) issues and questions with this:

- This feels inefficient and complicated.
- I don't know the actual possible values of operstate thus min. buffer size.
- I don't know the maximum length of an interface name thus min. buffer size.
- Does it make sense to there to be multiple (dis)connected  interfaces?
- Is this the behavior that I want it to actually have?
*/

/* Fills *globptr with the names of all wireless interfaces. Returns the
same value as glob(3). */
static int
globieee80211(glob_t *globptr)
{
	int rc;
	unsigned int i;
	char *lastslash;

	/* get all wireless adapters */
	rc = glob("/sys/class/ieee80211/*/device/net/*", 0, NULL, globptr);
	if (rc != 0)
		return rc;
	/* extract their names */
	for (i = 0; i < globptr->gl_pathc; i++) {
		lastslash = strrchr(globptr->gl_pathv[i], '/');
		strcpy(globptr->gl_pathv[i], lastslash + 1);
		/* we could even realloc  the string to be smaller */
	}
	return 0;
}

/* Scan one line from /proc/net/wireless. Returns EOF when everything
has been read. */
static int
readwirelessline(FILE *f, char name[MAX_INTERFACE_LEN], int *linkqualityptr)
{
	int rc;
	int dummy;

	/* XXX is this loop necessary? */
	do {
		rc = fscanf(f, "%" TOSTRING(MAX_INTERFACE_LEN) "[^: \t]: "
				"%*d %d. %*d. %*d %*d %*d %*d %*d %*d %d",
				name, linkqualityptr, &dummy);
	} while (rc != EOF && rc != 3);
	return rc;
}

/* Look for needle in *globptr. If found, free it and remove it from
the list. Returns 1 if it was found, and 0 if it wasn't. */
static int
deletefromglob(const char *needle, glob_t *globptr)
{
	unsigned int i;

	for (i = 0; i < globptr->gl_pathc; i++) {
		if (strcmp(globptr->gl_pathv[i], needle) != 0)
			continue;
		free(globptr->gl_pathv[i]);
		globptr->gl_pathv[i] = globptr->gl_pathv[--globptr->gl_pathc];
		return 1;
	}
	return 0;
}

/* Reads /proc/net/wireless, prints info it finds, and deletes found
interfaces from *globptr. Returns bytes written, or -1 if something went
wrong. */
static int
readwireless(FILE *stream, glob_t *globptr)
{
	int rc, total;
	int linkquality;
	FILE *wireless;
	static char name[MAX_INTERFACE_LEN];

	wireless = fopen("/proc/net/wireless", "r");
	if (wireless == NULL)
		return -1;
	/* ignore the first two lines */
	rc = fscanf(wireless, "%*[^\n]\n%*[^\n]\n");
	if (rc != 0) {
		fclose(wireless);
		return -1;
	}
	/* read the remaining lines */
	total = 0;
	while (readwirelessline(wireless, name, &linkquality) != EOF) {
		/* remove it from *globptr */
		deletefromglob(name, globptr);
		/* maybe print a seperator */
		if (total > 0)
			total += fprintf(stream, SEPARATOR);
		/* print its information */
		total += fprintf(stream, "%s %d", name, 100 * linkquality / 70);
	}
	/* done with that file */
	fclose(wireless);
	return total;
}

/* Prints the remaining interfaces in *globptr. Returns the number of
bytes written */
static int
printdisconnected(FILE *stream, glob_t *globptr, int needsep)
{
	int rc, total;
	unsigned int i;
	FILE *file;
	char operstate[16 /* I don't know the actual values of this */];
	static char path[PATH_MAX];

	total = 0;
	for (i = 0; i < globptr->gl_pathc; i++) {
		snprintf(path, PATH_MAX, "/sys/class/net/%s/operstate",
				globptr->gl_pathv[i]);
		file = fopen(path, "r");
		if (file == NULL)
			continue;
		rc = fscanf(file, "%s", operstate);
		fclose(file);
		if (rc != 1)
			continue;
		if (needsep)
			total += fprintf(stream, SEPARATOR);
		total += fprintf(stream, "%s %s", globptr->gl_pathv[i],
				operstate);
		needsep = 1;
	}
	return total;
}

static int
wifi(FILE *stream)
{
	int rc, total;
	glob_t globbuf;

	/* get all wireless interface names */
	rc = globieee80211(&globbuf);
	if (rc != 0)
		return 0;
	/* open the file that gives signal strengths */
	rc = readwireless(stream, &globbuf);
	if (rc == -1) {
		globfree(&globbuf);
		return 0;
	}
	total = rc;
	/* print information about disconnected interfaces */
	total += printdisconnected(stream, &globbuf, total != 0);
	/* done */
	globfree(&globbuf);
	return total;
}

/*
Disks.

The goal is to print out the utilization and available space for mount
points that correspond to partitions on hardware devices (I call these
"disks" even though they are just partitions...). It should also ignore
some partitions that (probably) don't matter, like /boot.

One of the challenges with this is that some partitions are mounted more
than once, like btrfs subvolumes. The solution I went with was to store
all of the disks that have been printed so far in an array, and ignore
any members of that array.

So, overall:

- Use /proc/mounts to find all mounted disks.
- Ignore undesired disks (see shouldignoredisk).
- Make sure the disk is not in the array.
- Make sure the array is not full.
- Print the disk's info.
- Add it to the set.

An urgent message is printed if the disk is almost full.

I read some busybox source before writing this section:
https://git.busybox.net/busybox/tree/util-linux/mount.c#n2320 and
https://git.busybox.net/busybox/tree/coreutils/df.c#n211.
*/

/* Predicate that matches disks from /dev that aren't under boot. */
static int
shouldignoredisk(const struct mntent *entptr)
{
	int devlen, bootlen;

	/* no magic numbers */
	devlen = strlen("/dev");
	bootlen = strlen("/boot");
	/* ignore file systems that aren't from /dev */
	if (strncmp(entptr->mnt_fsname, "/dev", devlen) != 0)
		return 1;
	/* ignore anything mounted below /boot */
	if (strncmp(entptr->mnt_dir, "/boot", bootlen) == 0)
		return 1;
	/* everything else is fine */
	return 0;
}

/* Linear search on an array of string pointers. Returns the string if
found, or NULL otherwise. */
static char *
strlsearch(const char *needle, char **haystack, int nstrings)
{
	int i;

	for (i = 0; i < nstrings; i++) {
		if (strcmp(needle, haystack[i]) == 0)
			return haystack[i];
	}
	return NULL;
}

/* printf("%d%c", *baseptr, *suffixptr) will be a human-readable
representation of bytes (parameter) bytes in binary units. */
static void
frombytes(unsigned long int bytes, int *baseptr, char *suffixptr)
{
	unsigned int i;
	static const char suffixes[] = {'B', 'k', 'M', 'G', 'T', 'P', 'E'};

	for (i = 0; i < LEN(suffixes) && bytes > 1024; i++, bytes /= 1024);
	*baseptr = (int)bytes;
	*suffixptr = i < LEN(suffixes) ? suffixes[i] : '?';
}

/* Get the disk's info from statvfs and print it. */
static int
printadisk(FILE *stream, struct mntent *ent)
{
	int rc, pct;
	unsigned long int total, avail, used;
	int availbase;
	char availsuffix;
	char *lastslash, *name, *ptr;
	struct statvfs statbuf;
	static char path[PATH_MAX];

	rc = statvfs(ent->mnt_dir, &statbuf);
	if (rc < 0)
		return 0;
	/* compute the stats we want to show */
	/* XXX will frsize ever != bsize? */
	total = statbuf.f_frsize * statbuf.f_blocks;
	avail = statbuf.f_frsize * statbuf.f_bavail;
	used = total - avail;
	pct = (int)(100ul * used / total);
	frombytes(avail, &availbase, &availsuffix);
	/* get the basename of the actual path */
	ptr = realpath(ent->mnt_fsname, path);
	if (ptr == NULL)
		return 0;
	lastslash = strrchr(path, '/');
	name = lastslash == NULL ? path : lastslash + 1;
	/* print its info */
	if (pct > 90)
		snprintf(urgentmsg, sizeof(urgentmsg), "%.128s is %d%% full "
				" (%d%c left)", name, pct, availbase,
				availsuffix);
	return fprintf(stream, "%s %d%% %d%c", name,
			pct, availbase, availsuffix);
}

static int
disks(FILE *stream)
{
	int ndisks, i, total;
	struct mntent *entptr;
	FILE *mounts;
	char *results[MAX_NUM_DISKS];

	total = ndisks = 0;
	/* busybox also uses /etc/mtab; is that the same? */
	mounts = setmntent("/proc/mounts", "r");
	if (mounts == NULL) {
		return 0;
	}
	/* busybox uses the gnu extension _r version */
	while (entptr = getmntent(mounts), entptr != NULL) {
		if (shouldignoredisk(entptr))
			continue;
		if (ndisks == MAX_NUM_DISKS)
			break;
		if (strlsearch(entptr->mnt_fsname, results, ndisks) != NULL)
			continue;
		results[ndisks++] = strdup(entptr->mnt_fsname);
		if (total > 0)
			total += fprintf(stream, SEPARATOR);
		total += printadisk(stream, entptr);
	}
	endmntent(mounts);
	for (i = 0; i < ndisks; i++)
		free(results[i]);
	return total;
}

/*
Memory utilization.

/proc/meminfo has all of the required info.
*/

static int
mem(FILE *stream)
{
	int rc;
	FILE *meminfo;
	long unsigned int pct, total, free, available;

	meminfo = fopen("/proc/meminfo", "r");
	if (meminfo == NULL)
		return 0;
	rc = fscanf(meminfo, "MemTotal: %lu kB "
			"MemFree: %lu kB "
			"MemAvailable: %lu kB ",
			&total, &free, &available);
	fclose(meminfo);
	if (rc != 3)
		return 0;
	pct = 100lu * (total - available) / total;
	return fprintf(stream, "mem %lu%%", pct);
}

/*
System load.

/proc/loadavg has all of the required info.
*/

static int
load(FILE *stream)
{
	int rc;
	float load;
	FILE *loadavg;

	loadavg = fopen("/proc/loadavg", "r");
	if (loadavg == NULL)
		return 0;
	rc = fscanf(loadavg, "%f", &load);
	fclose(loadavg);
	if (rc != 1)
		return 0;
	return fprintf(stream, "load %.2f", load);
}

/*
ALSA volume & mute status.

This part requires calling the correct sequence of libasound
functions. I got that sequence of functions from these patches:
https://tools.suckless.org/slstatus/patches/alsa/slstatus-alsa-4bd78c9.patch
and
https://tools.suckless.org/slstatus/patches/alsa/slstatus-alsa-mute-1.0.diff.

XXXX Not too sure on the correct cleanup operations.
*/

static int
alsa(FILE *stream)
{
	int rc, total;
	long int min, max, vol;
	int sw;
	snd_mixer_t *mixer;
	snd_mixer_selem_id_t *mixerid;
	snd_mixer_elem_t *elem;

	total = 0;
	/* XXX how much error checking is necessary? */
	rc = snd_mixer_open(&mixer, 0);
	if (rc != 0)
		return 0;
	rc = snd_mixer_attach(mixer, ALSA_DEVICE);
	if (rc != 0)
		goto close;
	rc = snd_mixer_selem_register(mixer, NULL, NULL);
	if (rc != 0)
		goto detach;
	rc = snd_mixer_load(mixer);
	if (rc != 0)
		goto detach;
	snd_mixer_selem_id_alloca(&mixerid);
	snd_mixer_selem_id_set_name(mixerid, ALSA_MIXER);
	snd_mixer_selem_id_set_index(mixerid, 0);
	elem = snd_mixer_find_selem(mixer, mixerid);
	if (elem == NULL)
		goto free;
	rc = snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
	if (rc != 0)
		goto free;
	rc = snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_MONO,
			&vol);
	if (rc != 0)
		goto free;
	rc = snd_mixer_selem_get_playback_switch(elem, 0, &sw);
	if (rc != 0)
		goto free;
	max -= min;
	vol -= min;
	if (sw)
		total = fprintf(stream, "vol %ld%%", 100l * vol / max);
	else
		total = fprintf(stream, "vol muted");
free:	snd_mixer_free(mixer);
detach:	snd_mixer_detach(mixer, ALSA_DEVICE);
close:	snd_mixer_close(mixer);
	return total;
}

/*
Battery states and capacities.

/sys/class/power_supply contains all power-related devices. Batteries
will have ./type read "Battery."

When a battery is very low (≤ 5%), an urgent message is printed.
*/

/* Convert the first letter of ./status to a symbol to print. */
static char
batterychar(char ch)
{
	switch (ch) {
	case 'C':
		return '+';
	case 'D':
		return '-';
	case 'I':
		return 'o';
	case 'F':
		return '=';
	default: /* 'U' */
		return '?';
	}
}

/* Print information about a device if it is a battery, and a separator
if needed. Some batteries have excessively long names (e.g., PlayStation
controllers), so long names are truncated after the final hyphen. */
static int
battery(FILE *stream, char *name, int needsep)
{
	int rc;
	int total;
	char *lastdash;
	FILE *f;
	int capacity;
	char ch;
	static char path[PATH_MAX];

	snprintf(path, PATH_MAX, BATTERY_PREFIX "%s/type", name);
	f = fopen(path, "r");
	if (f == NULL)
		return 0;
	ch = fgetc(f);
	fclose(f);
	if (ch != 'B')
		return 0;
	snprintf(path, PATH_MAX, BATTERY_PREFIX "%s/capacity", name);
	f = fopen(path, "r");
	if (f == NULL)
		return 0;
	rc = fscanf(f, "%d", &capacity);
	fclose(f);
	if (rc != 1)
		return 0;
	snprintf(path, PATH_MAX, BATTERY_PREFIX "%s/status", name);
	f = fopen(path, "r");
	if (f == NULL)
		return 0;
	ch = fgetc(f);
	fclose(f);
	if (ch == EOF)
		return 0;
	lastdash = strrchr(name, '-');
	if (lastdash != NULL)
		*lastdash = '\0';
	if (needsep)
		total = fprintf(stream, SEPARATOR);
	else
		total = 0;
	total += fprintf(stream, "%s %c%d%%", name, batterychar(ch), capacity);
	if (capacity < 5 && ch != 'C')
		snprintf(urgentmsg, sizeof(urgentmsg), "%s is running "
				"critically low (%d%%)", name, capacity);
	return total;
}

static int
batteries(FILE *stream)
{
	int rc, total, prefixlen, needsep;
	unsigned int i;
	glob_t globbuf;

	rc = glob(BATTERY_PREFIX "*", 0, NULL, &globbuf);
	if (rc != 0)
		return 0;
	prefixlen = strlen(BATTERY_PREFIX);
	for (i = 0, needsep = rc = total = 0; i < globbuf.gl_pathc; i++) {
		rc = battery(stream, &globbuf.gl_pathv[i][prefixlen], needsep);
		needsep = rc > 0 ? 1 : needsep;
		total += rc;
	}
	globfree(&globbuf);
	return total;
}

/*
Date & time.

ctime(3) has a nice format, but I don't care about seconds (which occur
after the last colon).
*/

static int
datetime(FILE *stream)
{
	time_t now;
	char buffer[26];

	time(&now);
	ctime_r(&now, buffer);
	*strrchr(buffer, ':') = '\0';
	return fprintf(stream, "%s", buffer);
}

/*
The array of status functions to iterate over.

XXX I'm not really consistent about whether these are "blocks" or
"monitor" or something else.
*/

/* A great example of C's ridiculous declaration syntax. */
static int (*const blocks[])(FILE *) = {
	wifi,
	disks,
	mem,
	load,
	alsa,
	batteries,
	datetime,
};

/*
Flashing urgent messages.

This function will flash the message in urgentmsg according to the
constants defined in the configuration macros. It does this using two
buffers for "on" and "off" states, but there's probably a more efficient
way to do this.
*/

static void
flashurgentmsg(void)
{
	int i, bytes;
	static char onbuf[2048 + 64], offbuf[2048 + 64];

	/* XXX there is probably a way to do this more efficiently and with
	less buffers */
	bytes = (int)strlen(urgentmsg);
	snprintf(onbuf, sizeof(onbuf), URGENT_PREFIX " %s " URGENT_SUFFIX,
			urgentmsg);
	snprintf(offbuf, sizeof(offbuf), URGENT_PREFIX " %*s " URGENT_SUFFIX,
			bytes, "");
	for (i = 0; i < URGENT_FLASHES; i++) {
		if (x) {
			eXStoreName(dpy, DefaultRootWindow(dpy), onbuf);
			XFlush(dpy);
		} else {
			printf("%s\n", onbuf);
			fflush(stdout);
		}
		nanosleep(TIMESPEC(URGENT_FLASH_ON), NULL);
		if (x) {
			eXStoreName(dpy, DefaultRootWindow(dpy), offbuf);
			XFlush(dpy);
		} else {
			printf("%s\n", offbuf);
			fflush(stdout);
		}
		nanosleep(TIMESPEC(URGENT_FLASH_OFF), NULL);
	}
}

/*
Iterate over blocks and insert separators where necessary.
*/

static int
printline(FILE *stream)
{
	unsigned int i;
	int rc, total;

	total = 2;
	fputc(' ', stream);
	for (i = 0, rc = 0; i < LEN(blocks); i++) {
		if (rc != 0) {
			total += fprintf(stream, SEPARATOR);
		}
		total += rc = blocks[i](stream);
	}
	fputc(' ', stream);
	return total;
}

/*
Main.
*/

int
main(int argc, char **argv)
{
	int i;
	FILE *memstream;
	struct sigaction action = {
		.sa_handler = onsignal,
	};

	/* Parse arguments */
	argv0 = argv[0];
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0) {
			fprintf(stderr, "astatus-0.0\n");
			return 0;
		} else if (strcmp(argv[i], "-1") == 0) {
			done = 1;
		} else if (XALLOWED && strcmp(argv[i], "-x") == 0) {
			x = 1;
		} else if (strcmp(argv[i], "-s") == 0) {
			x = 0;
		} else {
			fprintf(stderr, "usage: %s [-1] [-s]" XFLAG "\n",
					argv[0]);
			return 1;
		}
	}

	/* Install signal handlers. See comment about SA_RESTART. */
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGUSR1, &action, NULL);

	/* Set up X if needed. */
	if (x)
		dpy = eXOpenDisplay(NULL);

	/* The main loop. */
	do {
		/* Call printline and flush its output. */
		if (x) {
			memstream = efmemopen(xbuf, sizeof(xbuf), "w");
			printline(memstream);
			fclose(memstream);
			eXStoreName(dpy, DefaultRootWindow(dpy), xbuf);
			XFlush(dpy);
		} else {
			printline(stdout);
			printf("\n");
			fflush(stdout);
		}
		/* Either wait, or flash the urgent message if there is
		one (and clear it after). */
		if (urgentmsg[0] == '\0') {
			nanosleep(TIMESPEC(INTERVAL), NULL);
		} else {
			nanosleep(TIMESPEC(HOLD_TIME), NULL);
			flashurgentmsg();
			urgentmsg[0] = '\0';
		}
	} while (!done);

	/* Clear WM_NAME and close the display if using X. */
	if (x) {
		xbuf[0] = '\0';
		eXStoreName(dpy, DefaultRootWindow(dpy), xbuf);
		eXCloseDisplay(dpy);
	}

	return 0;
}
