#include <stdlib.h>
#include <signal.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#ifdef X
#include <X11/Xlib.h>
#else /* X */
#define DefaultRootWindow(x) (-1)
#define Display void
#define XCloseDisplay(x) (-1)
#define XFlush(dpy) NULL
#define XOpenDisplay(x) NULL
#define XStoreName(x, y, z) (-1)
#endif /* X */

#define LEN(arr) (sizeof(arr) / sizeof((arr)[0]))
#define STRINGIFY(X) #X
#define TOSTRING(X) STRINGIFY(X)
#ifdef X
#define XFLAG " [-x]"
#define XALLOWED 1
#else /* X */
#define XFLAG ""
#define XALLOWED 0
#endif /* X */
#define BATTERY_PREFIX "/sys/class/power_supply/"
#define MAX_INTERFACE_LEN 512
#define SEPARATOR "   "
#define INTERVAL 5

static int done = 0;
static int x = 0;
static Display *dpy;
static char xbuf[4096];

static void
onsignal(int signum)
{
	if (signum != SIGUSR1)
		done = 1;
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
disks(FILE *stream)
{
	int n;
	FILE *df;
	char ch;

	df = popen("df | awk '"
			"/^\\/dev\\/nvme/ {"
				"gsub(/\\/dev\\/|n.p./, \"\");"
				"total[$1] += $2;"
				"used[$1] += $3;"
			"}"
			"END {"
				"for (disk in total) {"
					"if (notfirst) printf \" \";"
					"notfirst = 1;"
					"sprintf(\"numfmt --to=iec-i %d\", 1024 * (total[disk] - used[disk])) | getline free;"
					"printf \"%s %d%% %s\", disk, 100 * used[disk] / total[disk], free;"
				"}"
			"}"
			"'", "r");
	if (df == NULL)
		return 0;
	/* XXX is this terribly inefficient? */
	for (ch = fgetc(df), n = 0; ch != EOF; ch = fgetc(df), n++)
		fputc(ch, stream);
	pclose(df);
	return n;
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
	int rc;
	FILE *amixer;
	int pct;
	char ch;

	amixer = popen("amixer sget Master | awk '"
			"/Front Left:/ {gsub(/[%[\\]]/, \"\"); print $5, $6}"
			"'", "r");
	if (amixer == NULL)
		return 0;
	rc = fscanf(amixer, "%d o%c", &pct, &ch);
	pclose(amixer);
	if (rc != 2)
		return 0;
	if (ch == 'n')
		return fprintf(stream, "vol %d%%", pct);
	return fprintf(stream, "vol muted");
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

static const struct timespec sleepinterval = {
	.tv_sec = INTERVAL,
	.tv_nsec = 0,
};

static int (*const blocks[])(FILE *) = {
	wifi,
	disks,
	mem,
	load,
	alsa,
	batteries,
	datetime,
};

static int
printline(FILE *stream)
{
	unsigned int i;
	int rc, total;

	for (i = 0, total = rc = 0; i < LEN(blocks); i++) {
		if (rc != 0) {
			total += fprintf(stream, SEPARATOR);
		}
		total += rc = blocks[i](stream);
	}
	return total;
}

int
main(int argc, char **argv)
{
	int i;
	int rc;
	FILE *memstream;
	struct sigaction action = {
		.sa_handler = onsignal,
	};

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
	if (x) {
		dpy = XOpenDisplay(NULL);
		if (dpy == NULL) {
			fprintf(stderr, "%s: XOpenDisplay: Failed to open "
					"display\n", argv[0]);
			return 1;
		}
	}
	do {
		if (x) {
			memstream = fmemopen(xbuf, sizeof(xbuf), "w");
			if (memstream == NULL) {
				perror("fmemopen");
				return 1;
			}
			printline(memstream);
			fclose(memstream);
			rc = XStoreName(dpy, DefaultRootWindow(dpy), xbuf);
			if (rc < 0) {
				fprintf(stderr, "%s: XStoreName: Allocation "
						"failed\n", argv[0]);
				return 1;
			}
			XFlush(dpy);
		} else {
			printline(stdout);
			printf("\n");
			fflush(stdout);
		}
		nanosleep(&sleepinterval, NULL);
	} while (!done);
	if (x) {
		xbuf[0] = '\0';
		rc = XStoreName(dpy, DefaultRootWindow(dpy), xbuf);
		if (rc < 0) {
			fprintf(stderr, "%s: XStoreName: Allocation "
					"failed\n", argv[0]);
			return 1;
		}
		rc = XCloseDisplay(dpy);
		if (rc < 0) {
			fprintf(stderr, "%s: XCloseDisplay: Failed to close "
					"display\n", argv[0]);
			return 1;
		}
	}
	return 0;
}
