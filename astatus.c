#include <stdlib.h>
#include <signal.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdarg.h>
#include <mntent.h>
#include <sys/statvfs.h>
#ifdef X
#include <X11/Xlib.h>
#else /* X */
#define DefaultRootWindow(x) (-1)
#define Display void
#define Window int
#define XCloseDisplay(x) (-1)
#define XFlush(dpy) NULL
#define XOpenDisplay(x) NULL
#define XStoreName(x, y, z) (-1)
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

#define LEN(arr) (sizeof(arr) / sizeof((arr)[0]))
#define STRINGIFY(X) #X
#define TOSTRING(X) STRINGIFY(X)
#define TIMESPEC(ms) (&(struct timespec){.tv_sec = (ms) / 1000, \
		.tv_nsec = (ms) % 1000 * 1e6})
#ifdef X
#define XFLAG " [-x]"
#define XALLOWED 1
#else /* X */
#define XFLAG ""
#define XALLOWED 0
#endif /* X */
#define BATTERY_PREFIX "/sys/class/power_supply/"
#define MAX_INTERFACE_LEN 512
#define MAX_NUM_DISKS 5
#define ALSA_DEVICE "default"
#define ALSA_MIXER "Master"
#define SEPARATOR " ┆ "
#define URGENT_PREFIX "!!!! Urgent message:"
#define URGENT_SUFFIX "!!!!"
#define INTERVAL 5000 /* ms between refreshes */
#define HOLD_TIME 1500 /* ms to display status when there is an urgent msg */
#define URGENT_FLASH_ON 100 /* ms to flash urgent message "on" for */
#define URGENT_FLASH_OFF 50 /* ms to flash urgent message "off" for */
#define URGENT_FLASHES 20 /* how many times to flash the urgent message */

static char *argv0 = "astatus";
static int done;
static int x = 0;
static Display *dpy;
static char xbuf[4096];
static char urgentmsg[2048];

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

static void
onsignal(int signum)
{
	if (signum != SIGUSR1)
		done = 1;
}

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
XXX is there a simpler way to do wifi? this is kinda a mess.
The goal is to display the signal strength for any connected interfaces
(can there be more than one?) and any disconnected interfaces. As far
as I can tell based on a hour or so of searching, it does not look
like there is a singular, simple interface that provides all of this
information. The solution I came up with is:

- Use glob on /sys/class/ieee80211 to get a list of wireless interfaces.
- Use /proc/net/wireless to get the signal strength of connected interfaces.
- Use /sys/class/net to get the states of remaining interfaces.

Here are the (potential) issues with this:

- This feels inefficient and complicated.
- I don't know the actual possible values of operstate thus min. buffer size.
- I don't know the maximum length of an interface name thus min. buffer size.
- Does it make sense to there to be multiple (dis)connected  interfaces?
- Is this the behavior that I want it to actually have?
*/

/* fills the glob_t with the names of all wireless interfaces.
returns the same value as glob(3). */
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

/* returns EOF when everything has been read */
static int
readwirelessline(FILE *f, char name[MAX_INTERFACE_LEN], int *linkqualityptr)
{
	int rc;
	int dummy;

	do {
		rc = fscanf(f, "%" TOSTRING(MAX_INTERFACE_LEN) "[^: \t]: "
				"%*d %d. %*d. %*d %*d %*d %*d %*d %*d %d",
				name, linkqualityptr, &dummy);
	} while (rc != EOF && rc != 3);
	return rc;
}

/* look for needle in *globptr. if found, free it and remove it from
the list. returns 1 if it was found, 0 if it wasn't. */
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

/* returns bytes written, or -1 if something went wrong. */
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

/* returns the number of bytes written */
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
		rc = snprintf(path, PATH_MAX, "/sys/class/net/%s/operstate",
				globptr->gl_pathv[i]);
		if (rc > PATH_MAX)
			continue;
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

static void
frombytes(unsigned long int bytes, int *baseptr, char *suffixptr)
{
	unsigned int i;
	static const char suffixes[] = {'B', 'k', 'M', 'G', 'T', 'P', 'E'};

	for (i = 0; i < LEN(suffixes) && bytes > 1024; i++, bytes /= 1024);
	*baseptr = (int)bytes;
	*suffixptr = i < LEN(suffixes) ? suffixes[i] : '?';
}

static int
printadisk(FILE *stream, struct mntent *ent)
{
	int rc, pct;
	unsigned long int total, avail, used;
	int availbase;
	char availsuffix;
	char *lastslash, *ptr;
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
	if (lastslash != NULL)
		strcpy(path, &lastslash[1]);
	/* print its info */
	if (pct > 90)
		snprintf(urgentmsg, sizeof(urgentmsg), "%.128s is %d%% full "
				" (%d%c left)", path, pct, availbase,
				availsuffix);
	return fprintf(stream, "%s %d%% %d%c", path,
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

	rc = snprintf(path, PATH_MAX, BATTERY_PREFIX "%s/type",
			name);
	if (rc >= PATH_MAX)
		return 0;
	f = fopen(path, "r");
	if (f == NULL)
		return 0;
	ch = fgetc(f);
	fclose(f);
	if (ch != 'B')
		return 0;
	rc = snprintf(path, PATH_MAX, BATTERY_PREFIX "%s/capacity",
			name);
	if (rc >= PATH_MAX)
		return 0;
	f = fopen(path, "r");
	if (f == NULL)
		return 0;
	rc = fscanf(f, "%d", &capacity);
	fclose(f);
	if (rc != 1)
		return 0;
	rc = snprintf(path, PATH_MAX, BATTERY_PREFIX "%s/status",
			name);
	if (rc >= PATH_MAX)
		return 0;
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

static int (*const blocks[])(FILE *) = {
	wifi,
	disks,
	mem,
	load,
	alsa,
	batteries,
	datetime,
};

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

int
main(int argc, char **argv)
{
	int i;
	FILE *memstream;
	struct sigaction action = {
		.sa_handler = onsignal,
	};

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
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGUSR1, &action, NULL);
	if (x)
		dpy = eXOpenDisplay(NULL);
	do {
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
		if (urgentmsg[0] == '\0') {
			nanosleep(TIMESPEC(INTERVAL), NULL);
		} else {
			nanosleep(TIMESPEC(HOLD_TIME), NULL);
			flashurgentmsg();
			urgentmsg[0] = '\0';
		}
	} while (!done);
	if (x) {
		xbuf[0] = '\0';
		eXStoreName(dpy, DefaultRootWindow(dpy), xbuf);
		eXCloseDisplay(dpy);
	}
	return 0;
}
